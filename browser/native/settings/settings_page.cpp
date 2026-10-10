#include "search_suggestion_service.h"
#include "settings_page.h"

#include "browser_profile_service.h"
#include "feedback_links.h"
#include "dalinira_blocker_service.h"
#include "song_finder_settings.h"
#include "tab_performance_manager.h"
#include "system_memory_pressure_monitor.h"
#include "desktop_tabs/tab_appearance.h"
#include "translate/translate_service.h"
#include "translate/language_detector.h"
#include "glow_toggle_switch.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "i18n/widget_i18n.h"
#include "engine/engine_backend.h"
#include "engine/engine_type.h"
#include "engine/engine_resolver.h"
#include "core/engine_ui_profile.h"
#include "core/keyboard_shortcut_service.h"
#include "newtab/new_tab_shortcuts.h"
#include "layout/browser_layout.h"
#include "layout/browser_layout_controller.h"
#include <QProcess>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QClipboard>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QVBoxLayout>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#include <QWebEngineProfile>

#include <algorithm>
#include <array>
#include <memory>

#include "settings_ui_helpers.h"

using namespace dalinira::settings_ui;

namespace {
void applySettingsAppearance(QWidget *page) {
  if (!page) return;
  QString style = settingsStyleSheet();
  if (page->palette().color(QPalette::Window).lightness() >= 128) {
    style += QStringLiteral(R"CSS(
      #settings-page, #settings-section, QScrollArea, QScrollArea > QWidget > QWidget { background:#f7f9fc; color:#202b37; }
      #settings-header, #settings-sidebar { background:#ffffff; border-color:#d8e0e9; color:#465769; }
      #settings-title, #settings-heading, #settings-row-title, #settings-mode-title { color:#17212b; }
      #settings-heading p, #settings-row-description, #settings-value, #settings-mode-desc, #settings-card-title { color:#647588; }
      #settings-card, #settings-mode-card { background:#ffffff; border-color:#d6dee8; }
      #settings-row:hover, #settings-mode-card:hover, #settings-sidebar::item:hover { background:#edf2f7; }
      #settings-mode-card[selected="true"], #settings-sidebar::item:selected { background:#e1efff; color:#17212b; border-color:#1873d1; }
      #settings-search, #settings-page QLineEdit, #settings-page QComboBox { background:#f4f7fa; color:#17212b; border-color:#c7d1dc; }
      #settings-search:focus, #settings-page QLineEdit:focus, #settings-page QComboBox:focus { background:#ffffff; border-color:#1873d1; }
      #settings-page QPushButton { background:#e8eef5; color:#243343; border-color:#c2ceda; }
      #settings-page QPushButton:hover { background:#dce6f0; border-color:#9eafc0; }
      #settings-row-separator { color:#e1e6ec; background:#e1e6ec; }
      #settings-shortcuts-intro { color:#647588; }
      #settings-shortcuts-status { color:#1a7f37; }
      #settings-shortcut-group { color:#17212b; }
      #settings-shortcut-divider { background:rgba(0,0,0,0.10); }
      #settings-shortcut-label { color:rgba(23,33,43,0.75); }
      #settings-shortcut-message { color:#647588; }
      #settings-shortcut-message[error="true"] { color:#b42318; }
      #settings-page QPushButton#settings-shortcut-edit { background:transparent; color:#17212b; border-color:rgba(0,0,0,0.16); }
      #settings-page QPushButton#settings-shortcut-edit:hover { background:rgba(0,0,0,0.03); border-color:rgba(0,0,0,0.30); }
      #settings-page QPushButton#settings-shortcut-edit:focus { border-color:#1873d1; }
      #settings-page QPushButton#settings-shortcut-edit[unset="true"] { color:rgba(23,33,43,0.35); }
      #settings-page QPushButton#settings-shortcut-edit[customized="true"] { color:#0f5fa8; }
      #settings-page QPushButton#settings-shortcut-edit[capturing="true"] { background:rgba(24,115,209,0.08); color:#0d3b66; border-color:#1873d1; }
      #settings-page QPushButton#settings-shortcut-edit[flash="valid"] { border-color:#1a7f37; }
      #settings-page QPushButton#settings-shortcut-edit[flash="invalid"] { border-color:#b42318; }
      #settings-page QPushButton#settings-shortcut-reset { background:transparent; border-color:transparent; }
      #settings-page QPushButton#settings-shortcut-reset:hover { background:rgba(0,0,0,0.06); }
    )CSS");
  }
  page->setStyleSheet(style);
}
}  // namespace


SettingsPage::SettingsPage(BrowserProfileService *profileService, Hooks hooks, QWidget *parent)
    : QWidget(parent), profileService_(profileService), hooks_(std::move(hooks)) {
  setObjectName(QStringLiteral("settings-page"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *header = new QWidget(this);
  header->setObjectName(QStringLiteral("settings-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(24, 14, 28, 14);
  headerLayout->setSpacing(20);

  title_ = new QLabel(I18n::text(QStringLiteral("menu.settings")), header);
  title_->setObjectName(QStringLiteral("settings-title"));
  headerLayout->addWidget(title_);
  headerLayout->addStretch(1);

  search_ = new QLineEdit(header);
  search_->setObjectName(QStringLiteral("settings-search"));
  search_->setPlaceholderText(I18n::text(QStringLiteral("settings.search_placeholder")));
  search_->setAccessibleName(I18n::text(QStringLiteral("settings.search_placeholder")));
  search_->setClearButtonEnabled(true);
  search_->setMaximumWidth(460);
  search_->addAction(BrowserIcons::icon(BrowserIcon::Search), QLineEdit::LeadingPosition);
  headerLayout->addWidget(search_, 1);
  root->addWidget(header);

  auto *body = new QWidget(this);
  auto *bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);

  sidebar_ = new QListWidget(body);
  sidebar_->setObjectName(QStringLiteral("settings-sidebar"));
  sidebar_->setAccessibleName(I18n::text(QStringLiteral("settings.categories_accessible")));
  sidebar_->setIconSize(QSize(18, 18));
  sidebar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sidebar_->setTextElideMode(Qt::ElideRight);
  sidebar_->setMinimumWidth(196);
  sidebar_->setMaximumWidth(226);
  sidebar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

  content_ = new QStackedWidget(body);
  content_->setObjectName(QStringLiteral("settings-content"));
  bodyLayout->addWidget(sidebar_);
  bodyLayout->addWidget(content_, 1);
  root->addWidget(body, 1);

  addCategory(Category::Startup, BrowserIcon::Startup, I18n::text(QStringLiteral("settings.startup.title")), I18n::text(QStringLiteral("settings.keywords.startup")), createStartupSection());
  addCategory(Category::Appearance, BrowserIcon::Appearance, I18n::text(QStringLiteral("settings.category.appearance")), I18n::text(QStringLiteral("settings.keywords.appearance")), createAppearanceSection());
  addCategory(Category::Performance, BrowserIcon::Performance, I18n::text(QStringLiteral("settings.category.performance")), I18n::text(QStringLiteral("settings.keywords.performance")), createPerformanceSection());
  addCategory(Category::Content, BrowserIcon::Content, I18n::text(QStringLiteral("settings.content.title")), I18n::text(QStringLiteral("settings.keywords.content")), createContentSection());
  addCategory(Category::Privacy, BrowserIcon::Privacy, I18n::text(QStringLiteral("settings.category.privacy")), I18n::text(QStringLiteral("settings.keywords.privacy")), createPrivacySection());
  addCategory(Category::Blocker, BrowserIcon::Privacy, QStringLiteral("DaliNira Blocker"), I18n::text(QStringLiteral("settings.keywords.blocker")), createBlockerSection());
  addCategory(Category::Search, BrowserIcon::Search, I18n::text(QStringLiteral("settings.category.search")), I18n::text(QStringLiteral("settings.keywords.search")), createSearchSection());
  addSidebarSeparator();
  addCategory(Category::Passwords, BrowserIcon::Password, I18n::text(QStringLiteral("menu.passwords")), I18n::text(QStringLiteral("settings.keywords.passwords")), createPasswordsSection());
  addCategory(Category::Languages, BrowserIcon::Language, I18n::text(QStringLiteral("settings.category.languages")), I18n::text(QStringLiteral("settings.keywords.languages")), createLanguagesSection());
  addCategory(Category::Downloads, BrowserIcon::Download, I18n::text(QStringLiteral("menu.downloads")), I18n::text(QStringLiteral("settings.keywords.downloads")), createDownloadsSection());
  addCategory(Category::Bookmarks, BrowserIcon::Bookmark, I18n::text(QStringLiteral("menu.bookmarks")), I18n::text(QStringLiteral("settings.keywords.bookmarks")), createBookmarksSection());
  addCategory(Category::History, BrowserIcon::History, I18n::text(QStringLiteral("menu.history")), I18n::text(QStringLiteral("settings.keywords.history")), createHistorySection());
  addCategory(Category::Accessibility, BrowserIcon::Accessibility, I18n::text(QStringLiteral("settings.category.accessibility")), I18n::text(QStringLiteral("settings.keywords.accessibility")), createAccessibilitySection());
  addCategory(Category::Shortcuts, BrowserIcon::Keyboard, I18n::text(QStringLiteral("settings.category.shortcuts")), I18n::text(QStringLiteral("settings.keywords.shortcuts")), createShortcutsSection());
  addSidebarSeparator();
  addCategory(Category::System, BrowserIcon::Settings, I18n::text(QStringLiteral("settings.category.system")), I18n::text(QStringLiteral("settings.keywords.system")), createSystemSection());
  addCategory(Category::Listening, BrowserIcon::Tools, QStringLiteral("Pulse"), I18n::text(QStringLiteral("settings.keywords.listening")), createListeningSection());
  addCategory(Category::Reset, BrowserIcon::Reset, I18n::text(QStringLiteral("settings.category.reset")), I18n::text(QStringLiteral("settings.keywords.reset")), createResetSection());
  addSidebarSeparator();
  addCategory(Category::About, BrowserIcon::Info, I18n::text(QStringLiteral("settings.about.title")), I18n::text(QStringLiteral("settings.keywords.about")), createAboutSection());

  applySettingsAppearance(this);
  connect(sidebar_, &QListWidget::currentRowChanged, this, &SettingsPage::selectCategory);
  connect(sidebar_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
    if (item) selectCategory(sidebar_->row(item));
  });
  connect(search_, &QLineEdit::textChanged, this, &SettingsPage::applyFilter);
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] { retranslateUi(); });
  if (profileService_) {
    connect(profileService_, &BrowserProfileService::historyChanged, this, [this] {
      if (refreshHistory_) refreshHistory_();
    });
    connect(profileService_, &BrowserProfileService::bookmarksChanged, this, [this] {
      if (refreshBookmarks_) refreshBookmarks_();
    });
  }
  setTabOrder(search_, sidebar_);
  setCategory(Category::Startup);
  retranslateUi();
}

void SettingsPage::refreshAppearance() { applySettingsAppearance(this); }

void SettingsPage::addCategory(Category category, BrowserIcon icon, const QString &name,
                               const QString &keywords, QWidget *section) {
  auto *scroll = new QScrollArea(content_);
  scroll->setObjectName(QStringLiteral("settings-scroll"));
  scroll->setAccessibleName(name);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(section);
  const int contentIndex = content_->addWidget(scroll);
  auto *item = new QListWidgetItem(BrowserIcons::icon(icon), name, sidebar_);
  item->setData(Qt::UserRole, contentIndex);
  item->setToolTip(name);
  const int sidebarRow = sidebar_->row(item);
  categoryIndexes_.insert(category, sidebarRow);
  contentSidebarRows_.insert(contentIndex, sidebarRow);
  searchKeywords_.insert(contentIndex, keywords);
}

void SettingsPage::retranslateUi() {
  dalinira::i18n::retranslateWidgetTree(this);
  const auto updateItem = [this](Category cat, const QString &textKey) {
    const int row = categoryIndexes_.value(cat, -1);
    if (row >= 0 && row < sidebar_->count()) {
      auto *item = sidebar_->item(row);
      if (item) {
        const QString txt = I18n::text(textKey);
        item->setText(txt);
        item->setToolTip(txt);
      }
    }
  };

  updateItem(Category::Startup, QStringLiteral("settings.category.startup"));
  updateItem(Category::Appearance, QStringLiteral("settings.category.appearance"));
  updateItem(Category::Performance, QStringLiteral("settings.category.performance"));
  updateItem(Category::Content, QStringLiteral("settings.category.content"));
  updateItem(Category::Privacy, QStringLiteral("settings.category.privacy"));
  updateItem(Category::Blocker, QStringLiteral("settings.category.blocker"));
  updateItem(Category::Search, QStringLiteral("settings.category.search"));
  updateItem(Category::Passwords, QStringLiteral("settings.category.passwords"));
  updateItem(Category::Bookmarks, QStringLiteral("settings.category.bookmarks"));
  updateItem(Category::History, QStringLiteral("settings.category.history"));
  updateItem(Category::Downloads, QStringLiteral("settings.category.downloads"));
  updateItem(Category::Languages, QStringLiteral("settings.category.languages"));
  updateItem(Category::Accessibility, QStringLiteral("settings.category.accessibility"));
  updateItem(Category::Shortcuts, QStringLiteral("settings.category.shortcuts"));
  updateItem(Category::System, QStringLiteral("settings.category.system"));
  updateItem(Category::Listening, QStringLiteral("settings.category.listening"));
  updateItem(Category::Reset, QStringLiteral("settings.category.reset"));
  updateItem(Category::About, QStringLiteral("settings.category.about"));

  if (title_) title_->setText(I18n::text(QStringLiteral("settings.title")));
  if (sidebar_) sidebar_->setAccessibleName(I18n::text(QStringLiteral("settings.categories_accessible")));

  const auto updateHeading = [this](Category category, const QString &titleKey,
                                    const QString &descriptionKey) {
    const int row = categoryIndexes_.value(category, -1);
    if (row < 0 || row >= sidebar_->count()) return;
    const int contentIndex = sidebar_->item(row)->data(Qt::UserRole).toInt();
    auto *page = content_->widget(contentIndex);
    auto *heading = page ? page->findChild<QLabel *>(QStringLiteral("settings-heading")) : nullptr;
    if (!heading) return;
    const QString title = I18n::text(titleKey);
    const QString description = I18n::text(descriptionKey);
    heading->setText(QStringLiteral("<h2>%1</h2><p>%2</p>").arg(title.toHtmlEscaped(), description.toHtmlEscaped()));
  };
  updateHeading(Category::Startup, QStringLiteral("settings.category.startup"), QStringLiteral("settings.description.startup"));
  updateHeading(Category::Appearance, QStringLiteral("settings.category.appearance"), QStringLiteral("settings.description.appearance"));
  updateHeading(Category::Performance, QStringLiteral("settings.category.performance"), QStringLiteral("settings.description.performance"));
  updateHeading(Category::Content, QStringLiteral("settings.category.content"), QStringLiteral("settings.description.content"));
  updateHeading(Category::Privacy, QStringLiteral("settings.category.privacy"), QStringLiteral("settings.description.privacy"));
  updateHeading(Category::Blocker, QStringLiteral("settings.category.blocker"), QStringLiteral("settings.description.blocker"));
  updateHeading(Category::Search, QStringLiteral("settings.category.search"), QStringLiteral("settings.description.search"));
  updateHeading(Category::Passwords, QStringLiteral("settings.category.passwords"), QStringLiteral("settings.description.passwords"));
  updateHeading(Category::Languages, QStringLiteral("settings.category.languages"), QStringLiteral("settings.language.subtitle"));
  updateHeading(Category::Downloads, QStringLiteral("settings.category.downloads"), QStringLiteral("settings.description.downloads"));
  updateHeading(Category::Bookmarks, QStringLiteral("settings.category.bookmarks"), QStringLiteral("settings.description.bookmarks"));
  updateHeading(Category::History, QStringLiteral("settings.category.history"), QStringLiteral("settings.description.history"));
  updateHeading(Category::Accessibility, QStringLiteral("settings.category.accessibility"), QStringLiteral("settings.description.accessibility"));
  updateHeading(Category::Shortcuts, QStringLiteral("settings.category.shortcuts"), QStringLiteral("settings.description.shortcuts"));
  updateHeading(Category::System, QStringLiteral("settings.category.system"), QStringLiteral("settings.description.system"));
  updateHeading(Category::Listening, QStringLiteral("settings.category.listening"), QStringLiteral("settings.description.listening"));
  updateHeading(Category::Reset, QStringLiteral("settings.category.reset"), QStringLiteral("settings.description.reset"));
  updateHeading(Category::About, QStringLiteral("settings.category.about"), QStringLiteral("settings.description.about"));

  if (search_) {
    search_->setPlaceholderText(I18n::text(QStringLiteral("settings.search_placeholder")));
  }

  if (uiLangCombo_) {
    uiLangCombo_->setItemText(0, dalinira::i18n::LanguageManager::instance().formatSystemLanguageLabel());
  }

  if (refreshHistory_) refreshHistory_();
  if (refreshBookmarks_) refreshBookmarks_();
  if (updatePrivacySubtitles_) updatePrivacySubtitles_();
  for (const auto &refresh : languageRefreshers_) refresh();
  if (search_ && !search_->text().isEmpty()) applyFilter(search_->text());
}

void SettingsPage::addSidebarSeparator() {
  auto *item = new QListWidgetItem(sidebar_);
  item->setFlags(Qt::NoItemFlags);
  item->setSizeHint(QSize(0, 8));
}

void SettingsPage::setCategory(Category category) {
  const int row = categoryIndexes_.value(category, -1);
  if (row >= 0) {
    if (!search_->text().isEmpty()) search_->clear();
    sidebar_->setCurrentRow(row);
    if (category == Category::Privacy && privacyStack_) {
      privacyStack_->setCurrentIndex(0);
      if (updatePrivacySubtitles_) updatePrivacySubtitles_();
    }
  }
}

void SettingsPage::refreshPreferences() {
  QSettings settings;
  if (auto *frequent = findChild<QCheckBox *>(QStringLiteral("settings-frequent-sites"))) {
    const QSignalBlocker blocker(frequent);
    frequent->setChecked(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showFrequentSites"), true).toBool());
  }
  if (auto *source = findChild<QComboBox *>(QStringLiteral("settings-top-sites-source"))) {
    const QSignalBlocker blocker(source);
    source->setCurrentIndex(source->findData(dalinira::newtab::shortcuts::source()));
  }
  if (auto *panel = findChild<QSlider *>(QStringLiteral("settings-frequent-panel-opacity"))) {
    const QSignalBlocker blocker(panel);
    panel->setValue(std::clamp(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesPanelOpacity"), 72).toInt(), 0, 100));
  }
  if (auto *icons = findChild<QSlider *>(QStringLiteral("settings-frequent-icon-opacity"))) {
    const QSignalBlocker blocker(icons);
    icons->setValue(std::clamp(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesIconOpacity"), 82).toInt(), 0, 100));
  }
  if (auto *restore = findChild<QPushButton *>(QStringLiteral("settings-restore-frequent-sites")))
    restore->setEnabled(!dalinira::ui_profile::getVisualValue(QStringLiteral("browser/hiddenFrequentSites")).toStringList().isEmpty());
  if (auto *autoHide = findChild<QCheckBox *>(QStringLiteral("settings-side-panel-auto-hide"))) {
    const QSignalBlocker blocker(autoHide);
    autoHide->setChecked(dalinira::layout::sidePanelAutoHideEnabled(dalinira::layout::currentLayoutMode()));
  }
  if (auto *engine = findChild<QComboBox *>(QStringLiteral("settings-search-engine"))) {
    const QSignalBlocker blocker(engine);
    engine->setCurrentText(settings.value(QStringLiteral("browser/searchEngine"), QStringLiteral("DuckDuckGo")).toString());
  }
  if (auto *suggestions = findChild<QCheckBox *>(QStringLiteral("settings-search-suggestions"))) {
    const QSignalBlocker blocker(suggestions);
    suggestions->setChecked(profileService_ ? profileService_->searchSuggestions()->isEnabled() : settings.value(QStringLiteral("browser/searchSuggestionsEnabled"), false).toBool());
  }
  if (auto *protection = findChild<QCheckBox *>(QStringLiteral("settings-blocker-protection"))) {
    const QSignalBlocker blocker(protection);
    protection->setChecked(profileService_ && profileService_->blockerService()
        ? profileService_->blockerService()->settings()->protectionEnabled() : true);
  }
  if (auto *discardToggle = findChild<QCheckBox *>(QStringLiteral("settings-discard-toggle"))) {
    const QSignalBlocker blocker(discardToggle);
    auto *pm = hooks_.performanceManager ? hooks_.performanceManager() : nullptr;
    discardToggle->setChecked(pm ? pm->isDiscardEnabled() : settings.value(QStringLiteral("performance/discardEnabled"), true).toBool());
  }
}

void SettingsPage::selectCategory(int row) {
  if (row < 0 || row >= sidebar_->count()) return;
  const QVariant contentIndex = sidebar_->item(row)->data(Qt::UserRole);
  if (contentIndex.isValid()) {
    content_->setCurrentIndex(contentIndex.toInt());
    if (privacyStack_ && row == categoryIndexes_.value(Category::Privacy, -1)) {
      privacyStack_->setCurrentIndex(0);
      if (updatePrivacySubtitles_) updatePrivacySubtitles_();
    }
    if (row == categoryIndexes_.value(Category::History, -1)) {
      if (refreshHistory_) refreshHistory_();
    }
    if (row == categoryIndexes_.value(Category::Bookmarks, -1)) {
      if (refreshBookmarks_) refreshBookmarks_();
    }
  }
}

QWidget *SettingsPage::createStartupSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.startup.title")), I18n::text(QStringLiteral("settings.startup.description")));
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.startup.behavior_card")));
  auto *restore = new QCheckBox(card); restore->setAccessibleName(I18n::text(QStringLiteral("settings.startup.restore_tabs_accessible")));
  restore->setChecked(QSettings().value(QStringLiteral("browser/restoreSession"), true).toBool());
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.startup.restore_tabs")), I18n::text(QStringLiteral("settings.startup.restore_tabs_desc")), restore, BrowserIcon::Startup, true));
  section.layout->addWidget(card); section.layout->addStretch();
  connect(restore, &QCheckBox::toggled, this, [](bool value) { QSettings().setValue(QStringLiteral("browser/restoreSession"), value); });
  return section.page;
}

QWidget *SettingsPage::createAppearanceSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.appearance")), I18n::text(QStringLiteral("settings.appearance.description")));

  auto *themeCard = makeCard(section.page, I18n::text(QStringLiteral("settings.appearance.theme")));
  auto *themeContainer = new QWidget(themeCard);
  auto *themeLayout = new QHBoxLayout(themeContainer);
  themeLayout->setContentsMargins(18, 14, 18, 14);
  themeLayout->setSpacing(22);
  auto *themeGroup = new QButtonGroup(themeContainer);
  const QString currentTheme = dalinira::layout::appearanceThemeToString(
      dalinira::layout::currentAppearanceTheme());
  const std::array<std::pair<QString, QString>, 3> themes{{
      {QStringLiteral("system"), I18n::text(QStringLiteral("settings.appearance.theme_system"))},
      {QStringLiteral("light"), I18n::text(QStringLiteral("settings.appearance.theme_light"))},
      {QStringLiteral("dark"), I18n::text(QStringLiteral("settings.appearance.theme_dark"))}}};
  for (int i = 0; i < static_cast<int>(themes.size()); ++i) {
    auto *radio = new QRadioButton(themes[i].second, themeContainer);
    radio->setObjectName(QStringLiteral("settings-theme-%1").arg(themes[i].first));
    radio->setChecked(themes[i].first == currentTheme);
    themeGroup->addButton(radio, i);
    themeLayout->addWidget(radio);
  }
  themeLayout->addStretch(1);
  connect(themeGroup, &QButtonGroup::idClicked, this, [this, themes](int index) {
    if (index < 0 || index >= static_cast<int>(themes.size())) return;
    const QString id = themes[index].first;
    const auto theme = dalinira::layout::appearanceThemeFromString(id);
    dalinira::layout::setPersistedAppearanceTheme(theme);
    dalinira::layout::applyApplicationTheme(qobject_cast<QApplication *>(qApp), theme);
    applySettingsAppearance(this);
    if (hooks_.setTheme) hooks_.setTheme(id);
  });
  addRow(themeCard, themeContainer);
  section.layout->addWidget(themeCard);

  // --------------------------------------------------------------------------
  // Card 1: SEKME GÖRÜNÜMÜ VE TARZLARI (Tab Styles)
  // --------------------------------------------------------------------------
  auto *tabStyleCard = makeCard(section.page, I18n::text(QStringLiteral("settings.appearance.tab_style_card")));
  auto *tabStyleContainer = new QWidget(tabStyleCard);
  tabStyleContainer->setObjectName(QStringLiteral("settings-tab-style-container"));
  auto *tabStyleLayout = new QVBoxLayout(tabStyleContainer);
  tabStyleLayout->setContentsMargins(18, 14, 18, 14);
  tabStyleLayout->setSpacing(10);

  struct TabStyleInfo {
    QString id;
    QString title;
    QString badge;
    QString description;
  };

  const std::vector<TabStyleInfo> tabStyles = {
    { QStringLiteral("chrome_curved"),
      I18n::text(QStringLiteral("common.standard")),
      QString(),
      I18n::text(QStringLiteral("settings.appearance.tab_style_standard_desc")) },
    { QStringLiteral("dalinira_signature"),
      I18n::text(QStringLiteral("settings.appearance.tab_style_signature")),
      I18n::text(QStringLiteral("common.recommended")),
      I18n::text(QStringLiteral("settings.appearance.tab_style_signature_desc")) },
    { QStringLiteral("floating_pill"),
      I18n::text(QStringLiteral("settings.appearance.tab_style_pill")),
      I18n::text(QStringLiteral("settings.appearance.badge_modern")),
      I18n::text(QStringLiteral("settings.appearance.tab_style_pill_desc")) },
    { QStringLiteral("dalinira_connected"),
      I18n::text(QStringLiteral("settings.appearance.tab_style_connected")),
      I18n::text(QStringLiteral("common.default")),
      I18n::text(QStringLiteral("settings.appearance.tab_style_connected_desc")) }
  };

  const QString currentStyleStr = dalinira::desktop_tabs::tabStylePreferenceValue(
      dalinira::desktop_tabs::tabStyleFromPreference(
          dalinira::ui_profile::getVisualValue(QStringLiteral("browser/tabStyle"),
                                               QStringLiteral("dalinira_connected")).toString()));

  QVector<QFrame *> styleFrameWidgets;
  QVector<QRadioButton *> styleRadioButtons;
  auto *styleBtnGroup = new QButtonGroup(tabStyleContainer);

  for (size_t i = 0; i < tabStyles.size(); ++i) {
    const auto &info = tabStyles[i];
    auto *frame = new ClickableFrame(tabStyleContainer);
    frame->setObjectName(QStringLiteral("settings-mode-card"));
    const bool isSelected = (info.id == currentStyleStr);
    frame->setProperty("selected", isSelected);
    frame->setCursor(Qt::PointingHandCursor);

    auto *fLayout = new QHBoxLayout(frame);
    fLayout->setContentsMargins(16, 12, 16, 12);
    fLayout->setSpacing(12);

    auto *radio = new QRadioButton(frame);
    radio->setChecked(isSelected);
    radio->setAccessibleName(info.title);
    radio->setAccessibleDescription(info.description);
    styleBtnGroup->addButton(radio, static_cast<int>(i));
    radio->setObjectName(QStringLiteral("settings-tab-style-%1").arg(info.id));
    styleRadioButtons.push_back(radio);
    frame->clicked = [radio] { radio->click(); };

    auto *textBox = new QWidget(frame);
    auto *tLayout = new QVBoxLayout(textBox);
    tLayout->setContentsMargins(0, 0, 0, 0);
    tLayout->setSpacing(2);

    auto *titleRow = new QWidget(textBox);
    auto *trLayout = new QHBoxLayout(titleRow);
    trLayout->setContentsMargins(0, 0, 0, 0);
    trLayout->setSpacing(8);

    auto *titleLabel = new QLabel(info.title, titleRow);
    titleLabel->setObjectName(QStringLiteral("settings-mode-title"));
    trLayout->addWidget(titleLabel);

    if (!info.badge.isEmpty()) {
      auto *badgeLabel = new QLabel(info.badge, titleRow);
      badgeLabel->setObjectName(QStringLiteral("settings-mode-badge"));
      trLayout->addWidget(badgeLabel);
    }
    trLayout->addStretch(1);

    auto *descLabel = new QLabel(info.description, textBox);
    descLabel->setObjectName(QStringLiteral("settings-mode-desc"));
    descLabel->setWordWrap(true);

    tLayout->addWidget(titleRow);
    tLayout->addWidget(descLabel);

    fLayout->addWidget(radio, 0, Qt::AlignVCenter);
    fLayout->addWidget(textBox, 1, Qt::AlignVCenter);

    tabStyleLayout->addWidget(frame);
    styleFrameWidgets.push_back(frame);
  }

  auto updateTabStyleSelection = [this, tabStyles, styleFrameWidgets, styleRadioButtons](int index) {
    if (index < 0 || index >= static_cast<int>(tabStyles.size())) return;
    const QString selectedId = tabStyles[index].id;
    for (int i = 0; i < static_cast<int>(styleFrameWidgets.size()); ++i) {
      const bool isSelected = (i == index);
      styleFrameWidgets[i]->setProperty("selected", isSelected);
      styleFrameWidgets[i]->style()->unpolish(styleFrameWidgets[i]);
      styleFrameWidgets[i]->style()->polish(styleFrameWidgets[i]);
      if (styleRadioButtons[i]->isChecked() != isSelected) {
        styleRadioButtons[i]->setChecked(isSelected);
      }
    }
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/tabStyle"), selectedId);
    if (hooks_.refreshTabStyle) hooks_.refreshTabStyle();
  };

  connect(styleBtnGroup, &QButtonGroup::idClicked, this, updateTabStyleSelection);

  addRow(tabStyleCard, tabStyleContainer);
  section.layout->addWidget(tabStyleCard);

  // --------------------------------------------------------------------------
  // Card: TARAYICI DÜZENİ VE PANELLER (Browser Layout & Panels)
  // --------------------------------------------------------------------------
  auto *layoutCard = makeCard(section.page, I18n::text(QStringLiteral("settings.appearance.layout_card")));
  auto *layoutContainer = new QWidget(layoutCard);
  layoutContainer->setObjectName(QStringLiteral("settings-layout-container"));
  auto *layoutVBox = new QVBoxLayout(layoutContainer);
  layoutVBox->setContentsMargins(18, 14, 18, 14);
  layoutVBox->setSpacing(10);

  struct LayoutOptionInfo {
    QString id;
    QString title;
    QString badge;
    QString description;
  };

  const std::vector<LayoutOptionInfo> layoutOptions = {
    { QStringLiteral("classic"),
      I18n::text(QStringLiteral("onboarding.layout_classic_name")),
      I18n::text(QStringLiteral("onboarding.layout_classic_badge")),
      I18n::text(QStringLiteral("onboarding.layout_classic_desc")) },
    { QStringLiteral("left_panel"),
      I18n::text(QStringLiteral("onboarding.layout_left_name")),
      I18n::text(QStringLiteral("onboarding.layout_left_badge")),
      I18n::text(QStringLiteral("onboarding.layout_left_desc")) },
    { QStringLiteral("right_panel"),
      I18n::text(QStringLiteral("onboarding.layout_right_name")),
      I18n::text(QStringLiteral("onboarding.layout_right_badge")),
      I18n::text(QStringLiteral("onboarding.layout_right_desc")) },
    { QStringLiteral("floating_panel"),
      I18n::text(QStringLiteral("onboarding.layout_floating_name")),
      I18n::text(QStringLiteral("onboarding.layout_floating_badge")),
      I18n::text(QStringLiteral("onboarding.layout_floating_desc")) }
  };

  const QString currentLayoutStr = dalinira::layout::layoutModeToString(dalinira::layout::currentLayoutMode());

  QVector<QFrame *> layoutFrameWidgets;
  QVector<QRadioButton *> layoutRadioButtons;
  auto *layoutBtnGroup = new QButtonGroup(layoutContainer);

  for (size_t i = 0; i < layoutOptions.size(); ++i) {
    const auto &info = layoutOptions[i];
    auto *frame = new ClickableFrame(layoutContainer);
    frame->setObjectName(QStringLiteral("settings-mode-card"));
    const bool isSelected = (info.id == currentLayoutStr);
    frame->setProperty("selected", isSelected);
    frame->setCursor(Qt::PointingHandCursor);

    auto *fLayout = new QHBoxLayout(frame);
    fLayout->setContentsMargins(16, 12, 16, 12);
    fLayout->setSpacing(12);

    auto *radio = new QRadioButton(frame);
    radio->setChecked(isSelected);
    radio->setAccessibleName(info.title);
    radio->setAccessibleDescription(info.description);
    layoutBtnGroup->addButton(radio, static_cast<int>(i));
    radio->setObjectName(QStringLiteral("settings-layout-%1").arg(info.id));
    layoutRadioButtons.push_back(radio);
    frame->clicked = [radio] { radio->click(); };

    auto *textBox = new QWidget(frame);
    auto *tLayout = new QVBoxLayout(textBox);
    tLayout->setContentsMargins(0, 0, 0, 0);
    tLayout->setSpacing(2);

    auto *titleRow = new QWidget(textBox);
    auto *trLayout = new QHBoxLayout(titleRow);
    trLayout->setContentsMargins(0, 0, 0, 0);
    trLayout->setSpacing(8);

    auto *titleLabel = new QLabel(info.title, titleRow);
    titleLabel->setObjectName(QStringLiteral("settings-mode-title"));
    trLayout->addWidget(titleLabel);

    if (!info.badge.isEmpty()) {
      auto *badgeLabel = new QLabel(info.badge, titleRow);
      badgeLabel->setObjectName(QStringLiteral("settings-mode-badge"));
      trLayout->addWidget(badgeLabel);
    }
    trLayout->addStretch(1);

    auto *descLabel = new QLabel(info.description, textBox);
    descLabel->setObjectName(QStringLiteral("settings-mode-desc"));
    descLabel->setWordWrap(true);

    tLayout->addWidget(titleRow);
    tLayout->addWidget(descLabel);

    fLayout->addWidget(radio, 0, Qt::AlignVCenter);
    fLayout->addWidget(textBox, 1, Qt::AlignVCenter);

    layoutVBox->addWidget(frame);
    layoutFrameWidgets.push_back(frame);
  }

  // Auto-hide side tabs (Zen-style): the docked side layouts only.
  auto *autoHide = new QCheckBox(layoutContainer);
  autoHide->setObjectName(QStringLiteral("settings-side-panel-auto-hide"));
  autoHide->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.side_tabs_auto_hide")));
  autoHide->setAccessibleDescription(I18n::text(QStringLiteral("settings.appearance.side_tabs_auto_hide_desc")));
  autoHide->setChecked(dalinira::layout::sidePanelAutoHideEnabled(dalinira::layout::currentLayoutMode()));
  const auto sideLayout = [](const QString &id) { return id != QLatin1String("classic"); };
  autoHide->setEnabled(sideLayout(currentLayoutStr));
  connect(autoHide, &QCheckBox::toggled, this, [this](bool enabled) {
    if (hooks_.setSidePanelAutoHide) hooks_.setSidePanelAutoHide(enabled);
    else dalinira::layout::setSidePanelAutoHideEnabled(enabled);
  });
  QWidget *autoHideRow = settingRow(layoutContainer, I18n::text(QStringLiteral("settings.appearance.side_tabs_auto_hide")),
                                    I18n::text(QStringLiteral("settings.appearance.side_tabs_auto_hide_desc")), autoHide);
  if (autoHideRow->layout()) autoHideRow->layout()->setContentsMargins(4, 8, 4, 0);
  layoutVBox->addWidget(autoHideRow);

  auto updateLayoutSelection = [this, layoutOptions, layoutFrameWidgets, layoutRadioButtons, autoHide, sideLayout](int index) {
    if (index < 0 || index >= static_cast<int>(layoutOptions.size())) return;
    const QString selectedId = layoutOptions[index].id;
    for (int i = 0; i < static_cast<int>(layoutFrameWidgets.size()); ++i) {
      const bool isSelected = (i == index);
      layoutFrameWidgets[i]->setProperty("selected", isSelected);
      layoutFrameWidgets[i]->style()->unpolish(layoutFrameWidgets[i]);
      layoutFrameWidgets[i]->style()->polish(layoutFrameWidgets[i]);
      if (layoutRadioButtons[i]->isChecked() != isSelected) {
        layoutRadioButtons[i]->setChecked(isSelected);
      }
    }
    dalinira::layout::setPersistedLayoutMode(dalinira::layout::layoutModeFromString(selectedId));
    autoHide->setEnabled(sideLayout(selectedId));
    {
      const QSignalBlocker blocker(autoHide);
      autoHide->setChecked(dalinira::layout::sidePanelAutoHideEnabled(dalinira::layout::layoutModeFromString(selectedId)));
    }
    if (hooks_.setLayoutMode) hooks_.setLayoutMode(selectedId);
  };

  connect(layoutBtnGroup, &QButtonGroup::idClicked, this, updateLayoutSelection);

  addRow(layoutCard, layoutContainer);
  section.layout->addWidget(layoutCard);

  // --------------------------------------------------------------------------
  // Card 2: YENİ SEKME (New Tab)
  // --------------------------------------------------------------------------
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.appearance.new_tab_card")));
  auto *frequent = new QCheckBox(card); frequent->setObjectName(QStringLiteral("settings-frequent-sites")); frequent->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.show_frequent_sites")));
  frequent->setChecked(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showFrequentSites"), true).toBool());
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.show_frequent_sites")), I18n::text(QStringLiteral("settings.appearance.show_frequent_sites_desc")), frequent, BrowserIcon::Content, true));
  auto *source = new QComboBox(card);
  source->setObjectName(QStringLiteral("settings-top-sites-source"));
  source->setAccessibleName(I18n::text(QStringLiteral("newtab.top_sites_source")));
  source->addItem(I18n::text(QStringLiteral("newtab.source_frequent")), QStringLiteral("frequent"));
  source->addItem(I18n::text(QStringLiteral("newtab.source_custom")), QStringLiteral("custom"));
  source->addItem(I18n::text(QStringLiteral("newtab.source_bookmarks")), QStringLiteral("bookmarks"));
  source->setCurrentIndex(source->findData(dalinira::newtab::shortcuts::source()));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("newtab.top_sites_source")), I18n::text(QStringLiteral("newtab.source_custom_desc")), source));
  QSlider *panel = nullptr; QLabel *panelValue = nullptr;
  QWidget *panelControl = sliderControl(&panel, &panelValue, std::clamp(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesPanelOpacity"), 72).toInt(), 0, 100), card);
  panel->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.panel_opacity")));
  panel->setObjectName(QStringLiteral("settings-frequent-panel-opacity"));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.panel_opacity")), I18n::text(QStringLiteral("settings.appearance.panel_opacity_desc")), panelControl));
  QSlider *icons = nullptr; QLabel *iconValue = nullptr;
  QWidget *iconControl = sliderControl(&icons, &iconValue, std::clamp(dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesIconOpacity"), 82).toInt(), 0, 100), card);
  icons->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.icon_opacity")));
  icons->setObjectName(QStringLiteral("settings-frequent-icon-opacity"));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.icon_opacity")), I18n::text(QStringLiteral("settings.appearance.icon_opacity_desc")), iconControl));
  auto *restore = new QPushButton(I18n::text(QStringLiteral("common.restore")), card); restore->setObjectName(QStringLiteral("settings-restore-frequent-sites")); restore->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.restore_removed_sites_accessible")));
  restore->setEnabled(!dalinira::ui_profile::getVisualValue(QStringLiteral("browser/hiddenFrequentSites")).toStringList().isEmpty());
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.removed_sites")), I18n::text(QStringLiteral("settings.appearance.removed_sites_desc")), restore, BrowserIcon::Reset, true));

  const bool legacyCards = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/cards"), true).toBool();
  const bool showDownloadsVal = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showDownloadsCard"), legacyCards).toBool();
  const bool showBlockedVal = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showBlockedCard"), legacyCards).toBool();
  const QString blockedCounterVal = dalinira::ui_profile::getVisualValue(
      QStringLiteral("browser/blockedCounterMode"), QStringLiteral("all_time")).toString();

  auto *showDownloads = new QCheckBox(card);
  showDownloads->setObjectName(QStringLiteral("settings-show-downloads-card"));
  showDownloads->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.show_downloads_card")));
  showDownloads->setChecked(showDownloadsVal);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.show_downloads_card")), I18n::text(QStringLiteral("settings.appearance.show_downloads_card_desc")), showDownloads, BrowserIcon::Download, true));

  auto *showBlocked = new QCheckBox(card);
  showBlocked->setObjectName(QStringLiteral("settings-show-blocked-card"));
  showBlocked->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.show_blocked_card")));
  showBlocked->setChecked(showBlockedVal);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.show_blocked_card")), I18n::text(QStringLiteral("settings.appearance.show_blocked_card_desc")), showBlocked, BrowserIcon::Privacy, true));

  auto *blockedCounter = new QComboBox(card);
  blockedCounter->setObjectName(QStringLiteral("settings-blocked-counter-mode"));
  blockedCounter->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.blocked_counter")));
  blockedCounter->addItem(I18n::text(QStringLiteral("settings.appearance.blocked_counter_session")), QStringLiteral("session"));
  blockedCounter->addItem(I18n::text(QStringLiteral("newtab.all_time")), QStringLiteral("all_time"));
  blockedCounter->setCurrentIndex(blockedCounterVal == QLatin1String("session") ? 0 : 1);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.blocked_counter")), I18n::text(QStringLiteral("settings.appearance.blocked_counter_desc")), blockedCounter, BrowserIcon::Privacy, true));

  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), card); reset->setProperty("danger", true); reset->setAccessibleName(I18n::text(QStringLiteral("settings.appearance.reset_new_tab_accessible")));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.reset_new_tab")), I18n::text(QStringLiteral("settings.appearance.reset_new_tab_desc")), reset));
  section.layout->addWidget(card); section.layout->addStretch();
  const auto save = [this, frequent, source, panel, icons, showDownloads, showBlocked, blockedCounter] {
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/topSitesSource"), source->currentData().toString());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showFrequentSites"), frequent->isChecked());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/frequentSitesPanelOpacity"), panel->value());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/frequentSitesIconOpacity"), icons->value());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showDownloadsCard"), showDownloads->isChecked());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showBlockedCard"), showBlocked->isChecked());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/blockedCounterMode"), blockedCounter->currentData().toString());
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/cards"), showDownloads->isChecked() || showBlocked->isChecked());
    if (hooks_.syncNewTabs) hooks_.syncNewTabs();
  };
  connect(panel, &QSlider::valueChanged, panelValue, [panelValue](int value) { panelValue->setText(QStringLiteral("%1%").arg(value)); });
  connect(icons, &QSlider::valueChanged, iconValue, [iconValue](int value) { iconValue->setText(QStringLiteral("%1%").arg(value)); });
  connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [save](int) { save(); });
  connect(frequent, &QCheckBox::toggled, this, [save](bool) { save(); });
  connect(panel, &QSlider::sliderReleased, this, save);
  connect(icons, &QSlider::sliderReleased, this, save);
  connect(showDownloads, &QCheckBox::toggled, this, [save](bool) { save(); });
  connect(showBlocked, &QCheckBox::toggled, this, [save](bool) { save(); });
  connect(blockedCounter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [save](int) { save(); });
  connect(restore, &QPushButton::clicked, this, [this, restore] { dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/hiddenFrequentSites")); restore->setEnabled(false); if (hooks_.syncNewTabs) hooks_.syncNewTabs(); });
  const auto resetAppearance = [save, frequent, source, panel, icons, restore, showDownloads, showBlocked, blockedCounter] {
    dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/hiddenFrequentSites"));
    source->setCurrentIndex(0);
    frequent->setChecked(true);
    panel->setValue(72);
    icons->setValue(82);
    restore->setEnabled(false);
    showDownloads->setChecked(true);
    showBlocked->setChecked(true);
    blockedCounter->setCurrentIndex(1);
    save();
  };
  connect(reset, &QPushButton::clicked, this, resetAppearance); connect(this, &SettingsPage::appearanceResetRequested, this, resetAppearance);
  return section.page;
}


QWidget *SettingsPage::createSearchSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.search")), I18n::text(QStringLiteral("settings.search.description")));
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.search.card")));
  auto *engine = new QComboBox(card); engine->setObjectName(QStringLiteral("settings-search-engine")); engine->setAccessibleName(I18n::text(QStringLiteral("settings.search.default_engine"))); engine->addItems({QStringLiteral("Google"), QStringLiteral("DuckDuckGo"), QStringLiteral("Startpage"), QStringLiteral("Mojeek")});
  if (profileService_) for (const auto &custom : profileService_->customSearchEngines()) engine->addItem(custom.name);
  engine->setCurrentText(hooks_.searchEngine ? hooks_.searchEngine() : QStringLiteral("DuckDuckGo"));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.search.default_engine")), I18n::text(QStringLiteral("settings.search.default_engine_desc")), engine, BrowserIcon::Search, true));
  auto *suggestions = new QCheckBox(card); suggestions->setObjectName(QStringLiteral("settings-search-suggestions")); suggestions->setAccessibleName(I18n::text(QStringLiteral("settings.search.enable_suggestions"))); suggestions->setChecked(profileService_ ? profileService_->searchSuggestions()->isEnabled() : QSettings().value(QStringLiteral("browser/searchSuggestionsEnabled"), false).toBool()); suggestions->setEnabled(!profileService_ || (!profileService_->isPrivate() && (!profileService_->profile() || !profileService_->profile()->isOffTheRecord())));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.search.suggestions")), I18n::text(QStringLiteral("settings.search.suggestions_desc")), suggestions));
  auto *clearSearches = new QPushButton(I18n::text(QStringLiteral("common.clear")), card); clearSearches->setProperty("danger", true); clearSearches->setAccessibleName(I18n::text(QStringLiteral("settings.search.clear_history_accessible"))); clearSearches->setEnabled(profileService_ && (!profileService_->profile() || !profileService_->profile()->isOffTheRecord()));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.search.history")), I18n::text(QStringLiteral("settings.search.history_desc")), clearSearches, BrowserIcon::History, true));
  section.layout->addWidget(card);

  auto *customCard = makeCard(section.page, I18n::text(QStringLiteral("settings.search.custom_engines_card")));
  auto *customList = new QListWidget(customCard);
  customList->setObjectName(QStringLiteral("custom-search-engine-list"));
  customList->setMinimumHeight(120);
  auto *buttons = new QWidget(customCard);
  auto *buttonsLayout = new QHBoxLayout(buttons);
  buttonsLayout->setContentsMargins(0, 0, 0, 0);
  auto *addCustom = new QPushButton(I18n::text(QStringLiteral("common.add")), buttons);
  auto *editCustom = new QPushButton(I18n::text(QStringLiteral("common.edit")), buttons);
  auto *removeCustom = new QPushButton(I18n::text(QStringLiteral("common.delete")), buttons);
  buttonsLayout->addWidget(addCustom);
  buttonsLayout->addWidget(editCustom);
  buttonsLayout->addWidget(removeCustom);
  buttonsLayout->addStretch();
  auto *customBox = new QWidget(customCard);
  auto *customLayout = new QVBoxLayout(customBox);
  customLayout->setContentsMargins(18, 10, 18, 14);
  customLayout->addWidget(customList);
  customLayout->addWidget(buttons);
  addRow(customCard, customBox);
  section.layout->addWidget(customCard);
  section.layout->addStretch();

  const auto refreshCustom = [this, customList, engine] {
    const QString selected = engine->currentText();
    customList->clear();
    while (engine->count() > 4) engine->removeItem(4);
    if (profileService_) {
      for (const auto &custom : profileService_->customSearchEngines()) {
        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(custom.name, custom.urlTemplate), customList);
        item->setData(Qt::UserRole, custom.name);
        item->setData(Qt::UserRole + 1, custom.urlTemplate);
        engine->addItem(custom.name);
      }
    }
    const int index = engine->findText(selected);
    engine->setCurrentIndex(index >= 0 ? index : 0);
  };
  refreshCustom();

  const auto editEngine = [this, customList, refreshCustom](bool editing) {
    if (!profileService_) return;
    QListWidgetItem *selected = customList->currentItem();
    if (editing && !selected) return;
    const QString oldName = editing ? selected->data(Qt::UserRole).toString() : QString{};
    bool ok = false;
    const QString name = QInputDialog::getText(this, I18n::text(QStringLiteral("settings.search.custom_engine_title")),
        I18n::text(QStringLiteral("settings.search.name_label")), QLineEdit::Normal, oldName, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const QString oldTemplate = editing ? selected->data(Qt::UserRole + 1).toString() : QStringLiteral("https://example.com/search?q=%s");
    const QString urlTemplate = QInputDialog::getText(this, I18n::text(QStringLiteral("settings.search.custom_engine_title")),
        I18n::text(QStringLiteral("settings.search.url_label")), QLineEdit::Normal, oldTemplate, &ok).trimmed();
    if (!ok) return;
    if (!profileService_->saveCustomSearchEngine(name, urlTemplate, oldName)) {
      QMessageBox::warning(this, I18n::text(QStringLiteral("settings.search.invalid_engine_title")),
          I18n::text(QStringLiteral("settings.search.invalid_engine_message")));
      return;
    }
    refreshCustom();
    if (hooks_.syncNewTabs) hooks_.syncNewTabs();
  };
  connect(engine, &QComboBox::currentTextChanged, this, [this](const QString &value) { QSettings().setValue(QStringLiteral("browser/searchEngine"), value); if (hooks_.setSearchEngine) hooks_.setSearchEngine(value); });
  connect(suggestions, &QCheckBox::toggled, this, [this](bool value) { if (profileService_) profileService_->setSearchSuggestionsEnabled(value); else QSettings().setValue(QStringLiteral("browser/searchSuggestionsEnabled"), value); if (hooks_.syncNewTabs) hooks_.syncNewTabs(); });
  connect(clearSearches, &QPushButton::clicked, this, [this] { if (profileService_) profileService_->clearSearchHistory(); if (hooks_.syncNewTabs) hooks_.syncNewTabs(); });
  connect(addCustom, &QPushButton::clicked, this, [editEngine] { editEngine(false); });
  connect(editCustom, &QPushButton::clicked, this, [editEngine] { editEngine(true); });
  connect(removeCustom, &QPushButton::clicked, this, [this, customList, refreshCustom] {
    if (!profileService_ || !customList->currentItem()) return;
    profileService_->removeCustomSearchEngine(customList->currentItem()->data(Qt::UserRole).toString());
    refreshCustom();
    if (hooks_.syncNewTabs) hooks_.syncNewTabs();
  });
  return section.page;
}

QWidget *SettingsPage::createPasswordsSection() {
  Section section = makeSection(I18n::text(QStringLiteral("menu.passwords")), I18n::text(QStringLiteral("settings.passwords.description")));
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.passwords.card")));
  auto *open = new QPushButton(I18n::text(QStringLiteral("settings.passwords.open_manager")), card);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.passwords.local_vault")), I18n::text(QStringLiteral("settings.passwords.local_vault_desc")), open, BrowserIcon::Password, true));
  section.layout->addWidget(card); section.layout->addStretch();
  connect(open, &QPushButton::clicked, this, [this] { emit navigateRequested(QUrl(QStringLiteral("dalinira://passwords"))); });
  return section.page;
}

QWidget *SettingsPage::createDownloadsSection() {
  Section section = makeSection(I18n::text(QStringLiteral("menu.downloads")), I18n::text(QStringLiteral("settings.downloads.description")));
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.downloads.preferences_card")));
  auto *folder = new QLineEdit(card); folder->setAccessibleName(I18n::text(QStringLiteral("settings.downloads.folder"))); folder->setReadOnly(true); folder->setMinimumWidth(190); folder->setMaximumWidth(360); folder->setText(profileService_->configuredDownloadDirectory()); folder->setPlaceholderText(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
  auto *choose = new QPushButton(I18n::text(QStringLiteral("common.change")), card); choose->setAccessibleName(I18n::text(QStringLiteral("settings.downloads.change_folder")));
  auto *folderControl = new QWidget(card); auto *folderLayout = new QHBoxLayout(folderControl); folderLayout->setContentsMargins(0,0,0,0); folderLayout->setSpacing(8); folderLayout->addWidget(folder, 1); folderLayout->addWidget(choose);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.downloads.location")), I18n::text(QStringLiteral("settings.downloads.location_desc")), folderControl, BrowserIcon::Folder, true));
  auto *ask = new QCheckBox(card); ask->setAccessibleName(I18n::text(QStringLiteral("settings.downloads.ask_location"))); ask->setChecked(profileService_->asksDownloadLocation());
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.downloads.ask_location")), I18n::text(QStringLiteral("settings.downloads.ask_location_desc")), ask));
  section.layout->addWidget(card);
  auto *activity = makeCard(section.page, I18n::text(QStringLiteral("settings.downloads.session_card")));
  auto *list = new QListWidget(activity); list->setObjectName(QStringLiteral("settings-data-list")); list->setAccessibleName(I18n::text(QStringLiteral("settings.downloads.session_list"))); list->setMinimumHeight(180);
  auto *container = new QWidget(activity); auto *containerLayout = new QVBoxLayout(container); containerLayout->setContentsMargins(18, 10, 18, 8); containerLayout->addWidget(list);
  addRow(activity, container);
  auto *policy = new QLabel(I18n::text(QStringLiteral("settings.downloads.policy_notice")), activity); policy->setObjectName(QStringLiteral("settings-row-description")); policy->setWordWrap(true); policy->setContentsMargins(18, 0, 18, 14); cardLayout(activity)->addWidget(policy);
  section.layout->addWidget(activity); section.layout->addStretch();
  const auto refresh = [this, list] { list->clear(); for (const BrowserDownloadEntry &entry : profileService_->recentDownloads()) { auto *item = new QListWidgetItem(BrowserIcons::icon(BrowserIcon::Download), QStringLiteral("%1\n%2").arg(entry.fileName, I18n::retranslated(entry.state)), list); item->setToolTip(entry.path); item->setSizeHint(QSize(0, 52)); } if (!list->count()) { auto *item = new QListWidgetItem(I18n::text(QStringLiteral("settings.downloads.empty")), list); item->setFlags(Qt::NoItemFlags); } }; refresh();
  connect(profileService_, &BrowserProfileService::downloadsChanged, this, refresh);
  languageRefreshers_.push_back(refresh);
  connect(choose, &QPushButton::clicked, this, [this, folder] {
    QString current = folder->text().trimmed();
    if (current.isEmpty() || !QDir(current).exists()) {
      current = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    current = QDir::cleanPath(current);
    if (!current.endsWith(QLatin1Char('/'))) current += QLatin1Char('/');
    QFileDialog dialog(this, I18n::text(QStringLiteral("settings.downloads.choose_folder")));
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    dialog.setDirectory(current);
    dialog.selectFile(QString{});
    dialog.setLabelText(QFileDialog::Accept, I18n::text(QStringLiteral("settings.bookmarks.choose_folder_title")));
    if (dialog.exec() == QDialog::Accepted) {
      const QStringList selected = dialog.selectedFiles();
      QString chosen = !selected.isEmpty() ? QDir::cleanPath(selected.first()) : dialog.directory().absolutePath();
      if (!chosen.isEmpty() && QFileInfo(chosen).isDir()) {
        folder->setText(chosen);
        profileService_->setDownloadDirectory(chosen);
      }
    }
  });
  connect(ask, &QCheckBox::toggled, this, [this](bool value) { profileService_->setAsksDownloadLocation(value); });
  return section.page;
}

QWidget *SettingsPage::createBookmarksSection() {
  Section section = makeSection(I18n::text(QStringLiteral("menu.bookmarks")), I18n::text(QStringLiteral("settings.bookmarks.description")));

  auto *barCard = makeCard(section.page, I18n::text(QStringLiteral("settings.bookmarks.bar_card")));
  auto *visibilityCombo = new QComboBox(barCard);
  visibilityCombo->setObjectName(QStringLiteral("settings-bookmark-bar-visibility"));
  visibilityCombo->addItem(I18n::text(QStringLiteral("settings.bookmarks.show_on_new_tab_only")), QStringLiteral("new_tab"));
  visibilityCombo->addItem(I18n::text(QStringLiteral("settings.bookmarks.show_always")), QStringLiteral("always"));
  visibilityCombo->addItem(I18n::text(QStringLiteral("settings.bookmarks.show_never")), QStringLiteral("never"));

  const QString curBmMode = QSettings().value(QStringLiteral("browser/bookmarkBarVisibility"), QStringLiteral("new_tab")).toString();
  int bmIdx = visibilityCombo->findData(curBmMode);
  if (bmIdx >= 0) visibilityCombo->setCurrentIndex(bmIdx);

  connect(visibilityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, visibilityCombo](int idx) {
    const QString mode = visibilityCombo->itemData(idx).toString();
    QSettings settings;
    settings.setValue(QStringLiteral("browser/bookmarkBarVisibility"), mode);
    settings.sync();
    if (hooks_.refreshBookmarkBarVisibility) hooks_.refreshBookmarkBarVisibility();
  });

  QWidget *barRow = settingRow(
      barCard,
      I18n::text(QStringLiteral("menu.bookmarks_bar")),
      I18n::text(QStringLiteral("settings.bookmarks.bar_visibility_desc")),
      visibilityCombo,
      BrowserIcon::Bookmark,
      true);
  addRow(barCard, barRow);
  // The description names the bookmarks-bar shortcut currently assigned.
  if (auto *barDescription = barRow->findChild<QLabel *>(QStringLiteral("settings-row-description"))) {
    const auto describeBar = [barDescription] {
      const QString key = dalinira::KeyboardShortcutService::primaryText(QStringLiteral("bookmarks.toggle_bar"));
      const QString text = I18n::text(QStringLiteral("settings.bookmarks.bar_visibility_desc"));
      barDescription->setText(key.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, key));
    };
    describeBar();
    connect(&dalinira::KeyboardShortcutService::instance(), &dalinira::KeyboardShortcutService::shortcutsChanged,
            barDescription, describeBar);
    languageRefreshers_.push_back(describeBar);
  }
  section.layout->addWidget(barCard);

  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.bookmarks.saved_card")));
  auto *tree = new QTreeWidget(card);
  tree->setObjectName(QStringLiteral("settings-bookmark-tree"));
  tree->setColumnCount(2);
  tree->setHeaderLabels({I18n::text(QStringLiteral("settings.bookmarks.column_name")), I18n::text(QStringLiteral("settings.bookmarks.column_location"))});
  tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  tree->setMinimumHeight(280);

  auto *addFolderBtn = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.new_folder_button")), card);
  auto *addBmBtn = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.new_bookmark_button")), card);
  auto *moveToFolderBtn = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.move_to_folder_button")), card);
  auto *remove = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.delete_selected")), card);
  remove->setProperty("danger", true);
  auto *importBtn = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.import_html")), card);
  auto *exportBtn = new QPushButton(I18n::text(QStringLiteral("settings.bookmarks.export_html")), card);

  auto *btnLayout = new QHBoxLayout;
  btnLayout->setContentsMargins(0, 0, 0, 0);
  btnLayout->setSpacing(8);
  btnLayout->addWidget(addFolderBtn);
  btnLayout->addWidget(addBmBtn);
  btnLayout->addWidget(moveToFolderBtn);
  btnLayout->addWidget(remove);
  btnLayout->addWidget(importBtn);
  btnLayout->addWidget(exportBtn);
  btnLayout->addStretch();

  auto *container = new QWidget(card);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(18, 10, 18, 14);
  layout->setSpacing(10);
  layout->addWidget(tree);
  layout->addLayout(btnLayout);
  addRow(card, container);
  section.layout->addWidget(card);
  section.layout->addStretch();

  const auto refresh = [this, tree] {
    tree->clear();
    if (!profileService_) return;

    const QStringList folders = profileService_->bookmarkFolders();
    const auto items = profileService_->bookmarkItems();

    QHash<QString, QTreeWidgetItem *> folderTreeItems;
    for (const QString &folderName : folders) {
      auto *folderItem = new QTreeWidgetItem(tree);
      folderItem->setText(0, folderName);
      folderItem->setText(1, I18n::text(QStringLiteral("settings.bookmarks.folder_marker")));
      folderItem->setIcon(0, BrowserIcons::icon(BrowserIcon::Folder));
      folderItem->setData(0, Qt::UserRole, folderName);
      folderItem->setData(0, Qt::UserRole + 2, true); // isFolder = true
      folderTreeItems.insert(folderName, folderItem);
    }

    for (const auto &item : items) {
      QTreeWidgetItem *parent = item.folder.isEmpty() ? nullptr : folderTreeItems.value(item.folder, nullptr);
      auto *bmItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
      const QString title = item.title.isEmpty() ? item.url.host() : item.title;
      bmItem->setText(0, title);
      bmItem->setText(1, item.url.toDisplayString());
      bmItem->setIcon(0, BrowserIcons::icon(BrowserIcon::Bookmark));
      bmItem->setData(0, Qt::UserRole, item.url);
      bmItem->setData(0, Qt::UserRole + 1, item.folder);
      bmItem->setData(0, Qt::UserRole + 2, false); // isFolder = false
      bmItem->setToolTip(0, item.url.toDisplayString());
      bmItem->setToolTip(1, item.url.toDisplayString());
    }

    tree->expandAll();
  };
  refreshBookmarks_ = refresh;
  refresh();

  connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
    if (!item) return;
    const bool isFolder = item->data(0, Qt::UserRole + 2).toBool();
    if (isFolder) {
      item->setExpanded(!item->isExpanded());
    } else {
      const QUrl url = item->data(0, Qt::UserRole).toUrl();
      if (url.isValid()) emit navigateRequested(url);
    }
  });

  connect(addFolderBtn, &QPushButton::clicked, this, [this, refresh] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, I18n::text(QStringLiteral("bookmark.add_folder_title")), I18n::text(QStringLiteral("bookmark.folder_name_prompt")), QLineEdit::Normal, QString(), &ok);
    if (ok && !name.trimmed().isEmpty() && profileService_) {
      profileService_->createBookmarkFolder(name.trimmed());
      refresh();
      if (hooks_.refreshBookmarks) hooks_.refreshBookmarks();
    }
  });

  connect(addBmBtn, &QPushButton::clicked, this, [this, refresh] {
    if (!profileService_) return;
    bool ok = false;
    const QString urlStr = QInputDialog::getText(this, I18n::text(QStringLiteral("settings.bookmarks.add_bookmark_title")), I18n::text(QStringLiteral("bookmark.url_label")), QLineEdit::Normal, QStringLiteral("https://"), &ok);
    if (!ok || urlStr.trimmed().isEmpty()) return;
    const QUrl url = QUrl::fromUserInput(urlStr.trimmed());
    if (!url.isValid()) return;

    QStringList folders = {I18n::text(QStringLiteral("settings.bookmarks.root_folder"))};
    folders.append(profileService_->bookmarkFolders());
    const QString chosen = QInputDialog::getItem(this, I18n::text(QStringLiteral("settings.bookmarks.choose_folder_title")), I18n::text(QStringLiteral("settings.bookmarks.target_folder")), folders, 0, false, &ok);
    const QString folder = (ok && chosen != I18n::text(QStringLiteral("settings.bookmarks.root_folder"))) ? chosen : QString();

    profileService_->addBookmark(url, url.host(), folder);
    refresh();
    if (hooks_.refreshBookmarks) hooks_.refreshBookmarks();
  });

  connect(moveToFolderBtn, &QPushButton::clicked, this, [this, tree, refresh] {
    auto *item = tree->currentItem();
    if (!item || !profileService_) return;
    const bool isFolder = item->data(0, Qt::UserRole + 2).toBool();
    if (isFolder) return;
    const QUrl url = item->data(0, Qt::UserRole).toUrl();
    if (!url.isValid()) return;

    QStringList folders = {I18n::text(QStringLiteral("settings.bookmarks.root_folder"))};
    folders.append(profileService_->bookmarkFolders());
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, I18n::text(QStringLiteral("settings.bookmarks.move_to_folder_title")), I18n::text(QStringLiteral("settings.bookmarks.target_folder")), folders, 0, false, &ok);
    if (ok) {
      const QString folder = (chosen == I18n::text(QStringLiteral("settings.bookmarks.root_folder"))) ? QString() : chosen;
      profileService_->moveBookmarkToFolder(url, folder);
      refresh();
      if (hooks_.refreshBookmarks) hooks_.refreshBookmarks();
    }
  });

  connect(remove, &QPushButton::clicked, this, [this, tree, refresh] {
    auto *item = tree->currentItem();
    if (!item || !profileService_) return;
    const bool isFolder = item->data(0, Qt::UserRole + 2).toBool();
    if (isFolder) {
      const QString folderName = item->data(0, Qt::UserRole).toString();
      profileService_->removeBookmarkFolder(folderName, true);
    } else {
      const QUrl url = item->data(0, Qt::UserRole).toUrl();
      if (url.isValid()) profileService_->removeBookmark(url);
    }
    refresh();
    if (hooks_.refreshBookmarks) hooks_.refreshBookmarks();
  });

  connect(importBtn, &QPushButton::clicked, this, [this, refresh] {
    const QString filePath = QFileDialog::getOpenFileName(this, I18n::text(QStringLiteral("settings.bookmarks.import_title")), QDir::homePath(), I18n::text(QStringLiteral("settings.bookmarks.html_filter")));
    if (filePath.isEmpty() || !profileService_) return;
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      profileService_->importBookmarksFromHtml(QString::fromUtf8(file.readAll()));
      refresh();
      if (hooks_.refreshBookmarks) hooks_.refreshBookmarks();
    }
  });

  connect(exportBtn, &QPushButton::clicked, this, [this] {
    if (!profileService_) return;
    const QString filePath = QFileDialog::getSaveFileName(this, I18n::text(QStringLiteral("settings.bookmarks.export_title")), QDir::homePath() + QStringLiteral("/bookmarks.html"), I18n::text(QStringLiteral("settings.bookmarks.html_filter")));
    if (filePath.isEmpty()) return;
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
      file.write(profileService_->exportBookmarksToHtml().toUtf8());
    }
  });

  return section.page;
}

QWidget *SettingsPage::createHistorySection() {
  Section section = makeSection(I18n::text(QStringLiteral("menu.history")), I18n::text(QStringLiteral("settings.history.description")));
  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.history.recent_card")));

  auto *searchEdit = new QLineEdit(card);
  searchEdit->setObjectName(QStringLiteral("history-search-input"));
  searchEdit->setPlaceholderText(I18n::text(QStringLiteral("settings.history.search_placeholder")));
  searchEdit->setClearButtonEnabled(true);

  auto *list = new QListWidget(card);
  list->setObjectName(QStringLiteral("history-list-widget"));
  list->setAccessibleName(I18n::text(QStringLiteral("settings.history.list")));
  list->setMinimumHeight(280);
  list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto *removeSelected = new QPushButton(I18n::text(QStringLiteral("settings.history.delete_selected")), card);
  removeSelected->setObjectName(QStringLiteral("history-delete-selected-button"));
  removeSelected->setProperty("danger", true);
  auto *clear = new QPushButton(I18n::text(QStringLiteral("menu.clear_history")), card);
  clear->setObjectName(QStringLiteral("history-clear-all-button"));
  clear->setProperty("danger", true);

  auto *btnLayout = new QHBoxLayout;
  btnLayout->setContentsMargins(0, 0, 0, 0);
  btnLayout->setSpacing(8);
  btnLayout->addWidget(removeSelected);
  btnLayout->addWidget(clear);
  btnLayout->addStretch();

  auto *container = new QWidget(card);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(18, 10, 18, 14);
  layout->setSpacing(10);
  layout->addWidget(searchEdit);
  layout->addWidget(list);
  layout->addLayout(btnLayout);
  addRow(card, container);
  section.layout->addWidget(card);
  section.layout->addStretch();

  const auto refresh = [this, list, searchEdit] {
    list->clear();
    if (!profileService_) return;
    const QString q = searchEdit->text().trimmed();
    const auto entries = q.isEmpty() ? profileService_->recentHistory() : profileService_->searchHistory(q);
    for (const BrowserHistoryEntry &entry : entries) {
      const QString title = entry.title.isEmpty() ? entry.url.host() : entry.title;
      const QString timeStr = entry.visitedAt.isValid()
          ? entry.visitedAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))
          : QStringLiteral("-");
      auto *item = new QListWidgetItem(BrowserIcons::icon(BrowserIcon::History),
          QStringLiteral("%1\n%2  ·  %3").arg(title, entry.url.toDisplayString(), timeStr), list);
      item->setData(Qt::UserRole, entry.url);
      item->setData(Qt::UserRole + 1, entry.visitedAt);
      item->setToolTip(entry.url.toDisplayString());
      item->setSizeHint(QSize(0, 56));
    }
    if (!list->count()) {
      auto *item = new QListWidgetItem(q.isEmpty() ? I18n::text(QStringLiteral("menu.history_empty")) : I18n::text(QStringLiteral("settings.history.no_matches")), list);
      item->setFlags(Qt::NoItemFlags);
    }
  };
  refreshHistory_ = refresh;
  refresh();

  connect(searchEdit, &QLineEdit::textChanged, this, [refresh] { refresh(); });
  connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    if (!item) return;
    const QUrl url = item->data(Qt::UserRole).toUrl();
    if (url.isValid()) emit navigateRequested(url);
  });

  const auto deleteCurrentItem = [this, list, refresh] {
    auto *item = list->currentItem();
    if (!item || !profileService_) return;
    const QUrl url = item->data(Qt::UserRole).toUrl();
    const QDateTime dt = item->data(Qt::UserRole + 1).toDateTime();
    if (url.isValid()) {
      profileService_->removeHistoryEntry(url, dt);
      refresh();
      if (hooks_.syncNewTabs) hooks_.syncNewTabs();
    }
  };

  connect(removeSelected, &QPushButton::clicked, this, deleteCurrentItem);
  connect(clear, &QPushButton::clicked, this, [this, refresh] {
    if (!profileService_) return;
    profileService_->clearHistory();
    refresh();
    if (hooks_.syncNewTabs) hooks_.syncNewTabs();
  });

  connect(list, &QListWidget::customContextMenuRequested, this, [this, list, deleteCurrentItem](const QPoint &pos) {
    auto *item = list->itemAt(pos);
    if (!item) return;
    const QUrl url = item->data(Qt::UserRole).toUrl();
    if (!url.isValid()) return;

    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral("QMenu{background:#1b232d;color:#e8eef5;border:1px solid #3a4857;border-radius:6px;padding:4px;} QMenu::item{padding:4px 20px;} QMenu::item:selected{background:#2a3644;}"));
    QAction *openAct = menu.addAction(I18n::text(QStringLiteral("menu.open")));
    QAction *copyAct = menu.addAction(I18n::text(QStringLiteral("context.copy_link_address")));
    menu.addSeparator();
    QAction *deleteAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Close), I18n::text(QStringLiteral("menu.delete_from_history")));

    QAction *chosen = menu.exec(list->mapToGlobal(pos));
    if (chosen == openAct) {
      emit navigateRequested(url);
    } else if (chosen == copyAct) {
      QGuiApplication::clipboard()->setText(url.toDisplayString());
    } else if (chosen == deleteAct) {
      list->setCurrentItem(item);
      deleteCurrentItem();
    }
  });

  return section.page;
}

QWidget *SettingsPage::createAccessibilitySection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.accessibility")), I18n::text(QStringLiteral("settings.accessibility.description")));
  section.layout->addWidget(placeholderPanel(section.page, BrowserIcon::Accessibility, I18n::text(QStringLiteral("settings.accessibility.placeholder_title")), I18n::text(QStringLiteral("settings.accessibility.placeholder_desc"))));
  section.layout->addStretch(); return section.page;
}

QWidget *SettingsPage::createSystemSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.system")), I18n::text(QStringLiteral("settings.system.description")));

  auto *engineCard = makeCard(section.page, I18n::text(QStringLiteral("settings.system.engine_card")));
  auto *engineCombo = new QComboBox(engineCard);
  engineCombo->setObjectName(QStringLiteral("settings-browser-engine"));
  auto *engineListView = new QListView(engineCombo);
  engineListView->setObjectName(QStringLiteral("settings-browser-engine-view"));
  engineListView->setMinimumHeight(108);
  engineCombo->setView(engineListView);
  engineCombo->setIconSize(QSize(24, 24));
  engineCombo->setMinimumWidth(270);
  engineCombo->setMaxVisibleItems(10);
  engineCombo->addItem(BrowserIcons::chromiumIcon(), QStringLiteral("Chromium (Blink)"), QStringLiteral("chromium"));
#if defined(DALINIRA_BUILD_WEBKIT)
  engineCombo->addItem(BrowserIcons::webkitIcon(), QStringLiteral("WebKit (WebCore)"), QStringLiteral("webkit"));
#endif

  const QString currentEnginePref = dalinira::engine::EngineResolver::getPersistedEngine();
  const int activeIdx = engineCombo->findData(currentEnginePref);
  if (activeIdx >= 0) {
    engineCombo->setCurrentIndex(activeIdx);
  } else {
    engineCombo->setCurrentIndex(0);
  }

  auto *restartBtn = new QPushButton(I18n::text(QStringLiteral("settings.system.apply_restart")), engineCard);
  restartBtn->setObjectName(QStringLiteral("settings-engine-restart"));
  restartBtn->setVisible(false);

  auto *statusLabel = new QLabel(engineCard);
  statusLabel->setObjectName(QStringLiteral("settings-engine-status"));
  statusLabel->setStyleSheet(QStringLiteral("color: #8ab4f8; font-size: 11px; padding-top: 4px;"));

  auto updateStatusLabel = [statusLabel, restartBtn](const QString &engineKey) {
    const QString runningEngine = (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit)
        ? QStringLiteral("webkit") : QStringLiteral("chromium");
    const bool changed = (engineKey != runningEngine);
    restartBtn->setVisible(changed);
    if (changed) {
      statusLabel->setText(I18n::text(QStringLiteral("settings.system.engine_changed_notice")));
    } else {
      if (engineKey == QLatin1String("webkit")) {
        statusLabel->setText(I18n::text(QStringLiteral("settings.system.active_engine_webkit")));
      } else {
        statusLabel->setText(I18n::text(QStringLiteral("settings.system.active_engine_chromium")));
      }
    }
  };
  updateStatusLabel(engineCombo->currentData().toString());

  connect(engineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [engineCombo, updateStatusLabel](int idx) {
    const QString selectedEngine = engineCombo->itemData(idx).toString();
    dalinira::engine::EngineResolver::setPersistedEngine(selectedEngine);
    updateStatusLabel(selectedEngine);
  });

  connect(restartBtn, &QPushButton::clicked, this, [engineCombo] {
    const QString selectedEngine = engineCombo->itemData(engineCombo->currentIndex()).toString();
    dalinira::engine::EngineResolver::setPersistedEngine(selectedEngine);

    const QString program = QCoreApplication::applicationFilePath();
    QStringList args = qApp->arguments().mid(1);
    // Remove any --engine override in args so newly saved engine is used
    for (int i = 0; i < args.size(); ++i) {
      if (args[i] == QLatin1String("--engine") && i + 1 < args.size()) {
        args.removeAt(i + 1);
        args.removeAt(i);
        break;
      }
    }
    QProcess::startDetached(program, args);
    qApp->quit();
  });

  auto *engineRow = settingRow(
      engineCard,
      I18n::text(QStringLiteral("settings.system.web_engine")),
      I18n::text(QStringLiteral("settings.system.engine_desc")),
      engineCombo,
      BrowserIcon::Settings,
      true);
  addRow(engineCard, engineRow);

  auto *statusRow = settingRow(
      engineCard,
      I18n::text(QStringLiteral("settings.system.engine_status")),
      I18n::text(QStringLiteral("settings.system.engine_status_desc")),
      statusLabel);
  addRow(engineCard, statusRow);

  auto *restartRow = settingRow(
      engineCard,
      I18n::text(QStringLiteral("common.restart")),
      I18n::text(QStringLiteral("settings.system.restart_desc")),
      restartBtn);
  addRow(engineCard, restartRow);

  section.layout->addWidget(engineCard);

  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.system.runtime_card")));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.system.profile_storage")), I18n::text(QStringLiteral("settings.system.profile_storage_desc")), nullptr, BrowserIcon::Settings, true));
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.system.security_policy")), I18n::text(QStringLiteral("settings.system.security_policy_desc")), nullptr));
  section.layout->addWidget(card);

  section.layout->addStretch();
  return section.page;
}

QWidget *SettingsPage::createResetSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.reset")), I18n::text(QStringLiteral("settings.reset.description")));
  auto *onboardingCard = makeCard(section.page, I18n::text(QStringLiteral("settings.reset.onboarding_card")));
  auto *reopenOnboarding = new QPushButton(
      I18n::text(QStringLiteral("settings.reset.onboarding_button")), onboardingCard);
  reopenOnboarding->setObjectName(QStringLiteral("settings-reopen-onboarding"));
  addRow(onboardingCard, settingRow(
      onboardingCard,
      I18n::text(QStringLiteral("settings.reset.onboarding_title")),
      I18n::text(QStringLiteral("settings.reset.onboarding_desc")),
      reopenOnboarding,
      BrowserIcon::Settings,
      true));
  section.layout->addWidget(onboardingCard);

  auto *card = makeCard(section.page, I18n::text(QStringLiteral("settings.appearance.new_tab_card")));
  auto *reset = new QPushButton(I18n::text(QStringLiteral("settings.appearance.reset_new_tab_accessible")), card); reset->setProperty("danger", true);
  addRow(card, settingRow(card, I18n::text(QStringLiteral("settings.appearance.reset_new_tab")), I18n::text(QStringLiteral("settings.reset.appearance_desc")), reset, BrowserIcon::Reset, true));
  section.layout->addWidget(card);

  auto *perfCard = makeCard(section.page, I18n::text(QStringLiteral("settings.reset.performance_card")));
  auto *resetPerf = new QPushButton(I18n::text(QStringLiteral("settings.reset.performance_button")), perfCard);
  resetPerf->setProperty("danger", true);
  addRow(perfCard, settingRow(
      perfCard,
      I18n::text(QStringLiteral("settings.reset.performance_title")),
      I18n::text(QStringLiteral("settings.reset.performance_desc")),
      resetPerf,
      BrowserIcon::Performance,
      true));
  section.layout->addWidget(perfCard);
  section.layout->addStretch();

  connect(reset, &QPushButton::clicked, this, &SettingsPage::appearanceResetRequested);
  connect(reopenOnboarding, &QPushButton::clicked, this, [this] {
    if (hooks_.reopenOnboarding) hooks_.reopenOnboarding();
  });
  connect(resetPerf, &QPushButton::clicked, this, [this] {
    auto *pm = hooks_.performanceManager ? hooks_.performanceManager() : nullptr;
    if (pm) {
      pm->setPolicyMode(dalinira::PerformancePolicyMode::Balanced);
      pm->setDiscardEnabled(true);
      pm->setSiteAllowlist({});
      pm->setSleepAfterMs(0);
      pm->setSleepMediaTabs(false);
    } else {
      QSettings s;
      s.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("balanced"));
      s.setValue(QStringLiteral("performance/discardEnabled"), true);
      s.remove(QStringLiteral("performance/siteAllowlist"));
      s.remove(QStringLiteral("performance/sleepAfterSeconds"));
      s.remove(QStringLiteral("performance/sleepMediaTabs"));
    }
    refreshPreferences();
  });
  return section.page;
}

QWidget *SettingsPage::createListeningSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.pulse.title")), I18n::text(QStringLiteral("settings.pulse.description")));

  auto *settings = new SongFinderSettings(section.page);

  // Card 1: Platform & Hassasiyet
  auto *card1 = makeCard(section.page, I18n::text(QStringLiteral("settings.pulse.platform_card")));

  auto *platformCombo = new QComboBox(card1);
  platformCombo->setObjectName(QStringLiteral("settings-listen-platform"));
  platformCombo->addItem(QStringLiteral("YouTube"), QStringLiteral("youtube"));
  platformCombo->addItem(QStringLiteral("YouTube Music"), QStringLiteral("ytmusic"));
  platformCombo->setCurrentIndex(settings->openPlatform() == SongFinderSettings::OpenPlatform::YouTubeMusic ? 1 : 0);
  connect(platformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [settings](int idx) {
    settings->setOpenPlatform(idx == 1 ? SongFinderSettings::OpenPlatform::YouTubeMusic : SongFinderSettings::OpenPlatform::YouTube);
    settings->save();
  });
  addRow(card1, settingRow(card1, I18n::text(QStringLiteral("pulse.open_found_song")), I18n::text(QStringLiteral("pulse.open_found_song_desc")), platformCombo));

  auto *sensitivityCombo = new QComboBox(card1);
  sensitivityCombo->setObjectName(QStringLiteral("settings-listen-sensitivity"));
  sensitivityCombo->addItem(I18n::text(QStringLiteral("pulse.sensitivity_normal")), QStringLiteral("normal"));
  sensitivityCombo->addItem(I18n::text(QStringLiteral("pulse.sensitivity_background")), QStringLiteral("background"));
  sensitivityCombo->addItem(I18n::text(QStringLiteral("pulse.sensitivity_max")), QStringLiteral("max"));
  sensitivityCombo->addItem(I18n::text(QStringLiteral("common.custom")), QStringLiteral("custom"));
  int sIdx = 1;
  switch (settings->sensitivityMode()) {
    case SongFinderSettings::SensitivityMode::Normal: sIdx = 0; break;
    case SongFinderSettings::SensitivityMode::Background: sIdx = 1; break;
    case SongFinderSettings::SensitivityMode::MaxAccuracy: sIdx = 2; break;
    case SongFinderSettings::SensitivityMode::Custom: sIdx = 3; break;
  }
  sensitivityCombo->setCurrentIndex(sIdx);

  auto *intervalSpin = new QSpinBox(card1);
  intervalSpin->setObjectName(QStringLiteral("settings-listen-interval"));
  intervalSpin->setRange(1, 120);
  intervalSpin->setSuffix(QStringLiteral(" sn"));
  intervalSpin->setValue(settings->requestIntervalSecs());

  auto *bufferSpin = new QSpinBox(card1);
  bufferSpin->setObjectName(QStringLiteral("settings-listen-buffer"));
  bufferSpin->setRange(4, 30);
  bufferSpin->setSuffix(QStringLiteral(" sn"));
  bufferSpin->setValue(settings->bufferSizeSecs());

  connect(sensitivityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [settings, intervalSpin, bufferSpin](int idx) {
    SongFinderSettings::SensitivityMode mode = SongFinderSettings::SensitivityMode::Custom;
    if (idx == 0) { mode = SongFinderSettings::SensitivityMode::Normal; intervalSpin->setValue(8); bufferSpin->setValue(10); }
    else if (idx == 1) { mode = SongFinderSettings::SensitivityMode::Background; intervalSpin->setValue(6); bufferSpin->setValue(12); }
    else if (idx == 2) { mode = SongFinderSettings::SensitivityMode::MaxAccuracy; intervalSpin->setValue(6); bufferSpin->setValue(16); }
    settings->setSensitivityMode(mode);
    settings->save();
  });

  connect(intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [settings](int val) {
    settings->setRequestIntervalSecs(val);
    settings->save();
  });

  connect(bufferSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [settings](int val) {
    settings->setBufferSizeSecs(val);
    settings->save();
  });

  addRow(card1, settingRow(card1, I18n::text(QStringLiteral("pulse.sensitivity")), I18n::text(QStringLiteral("pulse.sensitivity_desc")), sensitivityCombo));
  addRow(card1, settingRow(card1, I18n::text(QStringLiteral("pulse.request_interval")), I18n::text(QStringLiteral("pulse.request_interval_desc")), intervalSpin));
  addRow(card1, settingRow(card1, I18n::text(QStringLiteral("pulse.buffer_size")), I18n::text(QStringLiteral("pulse.buffer_size_desc")), bufferSpin));
  section.layout->addWidget(card1);

  // Card 2: Davranışlar
  auto *card2 = makeCard(section.page, I18n::text(QStringLiteral("settings.pulse.behavior_card")));

  auto *noDuplicatesCheck = new QCheckBox(card2);
  noDuplicatesCheck->setObjectName(QStringLiteral("settings-listen-noduplicates"));
  noDuplicatesCheck->setChecked(settings->noDuplicates());
  connect(noDuplicatesCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setNoDuplicates(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.skip_duplicates")), I18n::text(QStringLiteral("pulse.skip_duplicates_desc")), noDuplicatesCheck));

  auto *webFallbackCheck = new QCheckBox(card2);
  webFallbackCheck->setObjectName(QStringLiteral("settings-listen-webfallback"));
  webFallbackCheck->setChecked(settings->webMetadataFallback());
  connect(webFallbackCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setWebMetadataFallback(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.web_tab_fallback")), I18n::text(QStringLiteral("pulse.web_tab_fallback_desc")), webFallbackCheck));

  auto *autoStopCheck = new QCheckBox(card2);
  autoStopCheck->setObjectName(QStringLiteral("settings-listen-autostop"));
  autoStopCheck->setChecked(settings->autoStopOnResult());
  connect(autoStopCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setAutoStopOnResult(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.stop_on_found")), I18n::text(QStringLiteral("pulse.stop_on_found_desc")), autoStopCheck));

  auto *autoOpenCheck = new QCheckBox(card2);
  autoOpenCheck->setObjectName(QStringLiteral("settings-listen-autoopen"));
  autoOpenCheck->setChecked(settings->autoOpenOnResult());
  connect(autoOpenCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setAutoOpenOnResult(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.auto_search_on_found")), I18n::text(QStringLiteral("pulse.auto_search_on_found_desc")), autoOpenCheck));

  auto *rememberDeviceCheck = new QCheckBox(card2);
  rememberDeviceCheck->setObjectName(QStringLiteral("settings-listen-rememberdevice"));
  rememberDeviceCheck->setChecked(settings->rememberAudioDevice());
  connect(rememberDeviceCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setRememberAudioDevice(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.remember_source")), I18n::text(QStringLiteral("pulse.remember_source_desc")), rememberDeviceCheck));

  auto *autoPruneCheck = new QCheckBox(card2);
  autoPruneCheck->setObjectName(QStringLiteral("settings-listen-autoprune"));
  autoPruneCheck->setChecked(settings->autoPruneHistory());
  connect(autoPruneCheck, &QCheckBox::toggled, this, [settings](bool checked) {
    settings->setAutoPruneHistory(checked);
    settings->save();
  });
  addRow(card2, settingRow(card2, I18n::text(QStringLiteral("pulse.limit_history")), I18n::text(QStringLiteral("pulse.limit_history_desc")), autoPruneCheck));

  section.layout->addWidget(card2);
  section.layout->addStretch();
  return section.page;
}

QWidget *SettingsPage::createAboutSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.about.title")), I18n::text(QStringLiteral("settings.about.description")));
  auto *card = makeCard(section.page);
  auto *about = new QWidget(card);
  auto *layout = new QHBoxLayout(about);
  layout->setContentsMargins(24, 22, 24, 22);
  layout->setSpacing(20);

  auto *logo = new QLabel(about);
  logo->setPixmap(qApp->windowIcon().pixmap(72, 72));
  logo->setFixedSize(76, 76);
  logo->setAccessibleName(I18n::text(QStringLiteral("settings.about.logo")));

  QString engine = I18n::text(QStringLiteral("settings.about.engine_chromium"));
  if (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit) {
    auto *backend = dalinira::engine::EngineBackend::instance();
    engine = backend && !backend->versionString().isEmpty() ? backend->versionString() : QStringLiteral("WebKitGTK");
  } else if (profileService_ && profileService_->profile()) {
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("Chrome/([0-9.]+)")).match(profileService_->profile()->httpUserAgent());
    if (match.hasMatch()) engine = QStringLiteral("Chromium %1").arg(match.captured(1));
  }

  auto *details = new QLabel(about);
  dalinira::i18n::bindText(details, QStringLiteral("settings.about.details_html"),
                           {QStringLiteral(DALINIRA_BROWSER_VERSION), engine, QString::fromLatin1(qVersion())});
  details->setObjectName(QStringLiteral("settings-about-details"));
  details->setWordWrap(true);
  const auto navigateToAboutUrl = [this](const QUrl &url) { emit navigateRequested(url); };
  dalinira::about_links::wireLabel(details, this, navigateToAboutUrl);

  layout->addWidget(logo, 0, Qt::AlignTop);
  layout->addWidget(details, 1);
  addRow(card, about);
  section.layout->addWidget(card);

  // GitHub Hızlı Bağlantılar Kartı
  auto *linksCard = makeCard(section.page, I18n::text(QStringLiteral("settings.about.developer_card")));
  auto *btnContainer = new QWidget(linksCard);
  auto *btnLayout = new QHBoxLayout(btnContainer);
  btnLayout->setContentsMargins(18, 12, 18, 14);
  btnLayout->setSpacing(12);

  auto *profileBtn = new QPushButton(I18n::text(QStringLiteral("settings.about.github_profile")), linksCard);
  profileBtn->setObjectName(QStringLiteral("settings-about-github-profile"));
  profileBtn->setIcon(BrowserIcons::icon(BrowserIcon::Privacy));
  profileBtn->setCursor(Qt::PointingHandCursor);
  profileBtn->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  background-color: #21262d;"
      "  color: #c9d1d9;"
      "  border: 1px solid #30363d;"
      "  border-radius: 8px;"
      "  padding: 8px 16px;"
      "  font-weight: 600;"
      "  font-size: 13px;"
      "}"
      "QPushButton:hover {"
      "  background-color: #30363d;"
      "  color: #ffffff;"
      "  border-color: #8b949e;"
      "}"));
  dalinira::about_links::wireButton(profileBtn, dalinira::about_links::Destination::GitHubProfile,
                                    this, navigateToAboutUrl);

  auto *repoBtn = new QPushButton(I18n::text(QStringLiteral("settings.about.github_repo")), linksCard);
  repoBtn->setObjectName(QStringLiteral("settings-about-github-repository"));
  repoBtn->setIcon(BrowserIcons::icon(BrowserIcon::Save));
  repoBtn->setCursor(Qt::PointingHandCursor);
  repoBtn->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  background-color: #1f6feb;"
      "  color: #ffffff;"
      "  border: 1px solid #388bfd;"
      "  border-radius: 8px;"
      "  padding: 8px 16px;"
      "  font-weight: 600;"
      "  font-size: 13px;"
      "}"
      "QPushButton:hover {"
      "  background-color: #388bfd;"
      "}"));
  dalinira::about_links::wireButton(repoBtn, dalinira::about_links::Destination::Repository,
                                    this, navigateToAboutUrl);

  btnLayout->addWidget(profileBtn);
  btnLayout->addWidget(repoBtn);
  btnLayout->addStretch();
  addRow(linksCard, btnContainer);

  section.layout->addWidget(linksCard);

  auto *feedbackCard = makeCard(section.page, I18n::text(QStringLiteral("settings.about.feedback_card")));
  auto *feedbackContainer = new QWidget(feedbackCard);
  auto *feedbackLayout = new QVBoxLayout(feedbackContainer);
  feedbackLayout->setContentsMargins(18, 12, 18, 14);
  feedbackLayout->setSpacing(10);

  auto *feedbackButtons = new QWidget(feedbackContainer);
  auto *feedbackButtonsLayout = new QHBoxLayout(feedbackButtons);
  feedbackButtonsLayout->setContentsMargins(0, 0, 0, 0);
  feedbackButtonsLayout->setSpacing(12);

  auto *bugReport = new QPushButton(
      I18n::text(QStringLiteral("settings.about.feedback_bug_report")), feedbackButtons);
  bugReport->setObjectName(QStringLiteral("settings-feedback-bug-report"));
  bugReport->setCursor(Qt::PointingHandCursor);
  bugReport->setAccessibleName(bugReport->text());

  auto *featureRequest = new QPushButton(
      I18n::text(QStringLiteral("settings.about.feedback_feature_request")), feedbackButtons);
  featureRequest->setObjectName(QStringLiteral("settings-feedback-feature-request"));
  featureRequest->setCursor(Qt::PointingHandCursor);
  featureRequest->setAccessibleName(featureRequest->text());

  auto *sendEmail = new QPushButton(
      I18n::text(QStringLiteral("settings.about.feedback_send_email")), feedbackButtons);
  sendEmail->setObjectName(QStringLiteral("settings-feedback-send-email"));
  sendEmail->setCursor(Qt::PointingHandCursor);
  sendEmail->setAccessibleName(sendEmail->text());
  sendEmail->setToolTip(dalinira::about_links::feedbackEmailAddress());
  const bool lightSettings = palette().color(QPalette::Window).lightness() >= 128;
  sendEmail->setIcon(lightSettings
      ? BrowserIcons::tintedIcon(BrowserIcon::Mail, QColor(QStringLiteral("#465769")),
                                 QColor(QStringLiteral("#17212b")), QColor(QStringLiteral("#a7b2bf")))
      : BrowserIcons::icon(BrowserIcon::Mail));
  sendEmail->setIconSize(QSize(16, 16));

  feedbackButtonsLayout->addWidget(bugReport);
  feedbackButtonsLayout->addWidget(featureRequest);
  feedbackButtonsLayout->addWidget(sendEmail);
  feedbackButtonsLayout->addStretch();

  auto *privacyNote = new QLabel(
      I18n::text(QStringLiteral("settings.about.feedback_privacy_note")), feedbackContainer);
  privacyNote->setObjectName(QStringLiteral("settings-row-description"));
  privacyNote->setWordWrap(true);

  feedbackLayout->addWidget(feedbackButtons);
  feedbackLayout->addWidget(privacyNote);
  addRow(feedbackCard, feedbackContainer);
  section.layout->addWidget(feedbackCard);

  dalinira::about_links::wireButton(bugReport, dalinira::about_links::Destination::BugReport,
                                    this, navigateToAboutUrl);
  dalinira::about_links::wireButton(featureRequest,
                                    dalinira::about_links::Destination::FeatureRequest,
                                    this, navigateToAboutUrl);
  // Version and engine name only; the user reviews the draft and presses Send.
  dalinira::about_links::wireEmailButton(
      sendEmail, this,
      [engine] {
        return dalinira::about_links::emailUrl(
            I18n::text(QStringLiteral("settings.about.feedback_email_subject")),
            I18n::text(QStringLiteral("settings.about.feedback_email_body"))
                .arg(QStringLiteral(DALINIRA_BROWSER_VERSION), engine));
      },
      [this](const QUrl &url) { emit emailRequested(url); });

  section.layout->addStretch();
  return section.page;
}

void SettingsPage::applyFilter(const QString &query) {
  const QString term = query.trimmed().toCaseFolded();
  int firstMatchRow = -1;
  for (int contentIndex = 0; contentIndex < content_->count(); ++contentIndex) {
    // Keywords follow the active UI language; English keywords always match
    // too, so a search typed in English works in every language.
    const QString keywords = searchKeywords_.value(contentIndex);
    const QString keywordKey = dalinira::i18n::LanguageManager::instance().keyForText(keywords);
    QString searchable = keywordKey.isEmpty()
        ? keywords
        : I18n::text(keywordKey) + QLatin1Char(' ')
              + dalinira::i18n::LanguageManager::instance().translateIn(QStringLiteral("en"), keywordKey);
    const int row = contentSidebarRows_.value(contentIndex, -1);
    if (row >= 0) searchable += QLatin1Char(' ') + sidebar_->item(row)->text();
    QWidget *page = content_->widget(contentIndex);
    for (const QLabel *label : page->findChildren<QLabel *>()) searchable += QLatin1Char(' ') + label->text();
    for (const QAbstractButton *button : page->findChildren<QAbstractButton *>()) searchable += QLatin1Char(' ') + button->text();
    for (const QComboBox *box : page->findChildren<QComboBox *>()) for (int option = 0; option < box->count(); ++option) searchable += QLatin1Char(' ') + box->itemText(option);
    for (const QLineEdit *edit : page->findChildren<QLineEdit *>()) searchable += QLatin1Char(' ') + edit->placeholderText() + QLatin1Char(' ') + edit->text();
    const int sidebarRow = contentSidebarRows_.value(contentIndex, -1);
    const bool match = term.isEmpty() || searchable.toCaseFolded().contains(term);
    if (sidebarRow >= 0) sidebar_->item(sidebarRow)->setHidden(!match);
    if (match && firstMatchRow < 0) firstMatchRow = sidebarRow;
  }
  if (!term.isEmpty() && firstMatchRow >= 0) sidebar_->setCurrentRow(firstMatchRow);
}
