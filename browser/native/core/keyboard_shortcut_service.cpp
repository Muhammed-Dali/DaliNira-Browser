#include "keyboard_shortcut_service.h"

#include <QDebug>
#include <QSet>
#include <QSettings>

namespace dalinira {

namespace {

constexpr int kStorageVersion = 1;
const QString kGroup = QStringLiteral("shortcuts");
const QString kVersionKey = QStringLiteral("version");

QKeySequence keys(QKeyCombination combination) { return QKeySequence(combination); }

ShortcutActionDefinition action(const char *id, const char *category, const char *labelKey,
                                QList<QKeySequence> defaults = {}) {
  return {QString::fromLatin1(id), QString::fromLatin1(category), QString::fromLatin1(labelKey),
          std::move(defaults)};
}

// Groups and their order follow Zen's keyboard-shortcuts page (window & tab
// management first, then navigation, search & find, page operations, history
// & bookmarks, media & display), with DaliNira's own features last. Defaults
// are the shortcuts DaliNira already bound or its menus advertised, plus the
// conventional browser keys (zoom, home, close window, Ctrl+1…8/9 tab
// selection, Ctrl+PgUp/PgDn). Fixed values, not platform StandardKeys, so a
// profile reads the same on every desktop.
QList<ShortcutActionDefinition> buildRegistry() {
  using Qt::ALT;
  using Qt::CTRL;
  using Qt::SHIFT;
  QList<ShortcutActionDefinition> registry{
      action("window.new", "windows_tabs", "shortcuts.action.window_new", {keys(CTRL | Qt::Key_N)}),
      action("window.new_incognito", "windows_tabs", "shortcuts.action.window_new_incognito",
             {keys(CTRL | SHIFT | Qt::Key_N)}),
      action("tabs.new", "windows_tabs", "shortcuts.action.tabs_new", {keys(CTRL | Qt::Key_T)}),
      action("window.fullscreen", "windows_tabs", "shortcuts.action.window_fullscreen", {keys(Qt::Key_F11)}),
      action("tabs.close", "windows_tabs", "shortcuts.action.tabs_close", {keys(CTRL | Qt::Key_W)}),
      action("window.close", "windows_tabs", "shortcuts.action.window_close", {keys(CTRL | SHIFT | Qt::Key_W)}),
      action("tabs.reopen_closed", "windows_tabs", "shortcuts.action.tabs_reopen_closed",
             {keys(CTRL | SHIFT | Qt::Key_T)}),
      action("tabs.next", "windows_tabs", "shortcuts.action.tabs_next",
             {keys(CTRL | Qt::Key_Tab), keys(CTRL | Qt::Key_PageDown)}),
      action("tabs.previous", "windows_tabs", "shortcuts.action.tabs_previous",
             {keys(CTRL | SHIFT | Qt::Key_Tab), keys(CTRL | Qt::Key_PageUp)}),
  };
  static const char *const selectTab[] = {"tabs.select_1", "tabs.select_2", "tabs.select_3", "tabs.select_4",
                                          "tabs.select_5", "tabs.select_6", "tabs.select_7", "tabs.select_8"};
  static const char *const selectTabLabel[] = {
      "shortcuts.action.tabs_select_1", "shortcuts.action.tabs_select_2", "shortcuts.action.tabs_select_3",
      "shortcuts.action.tabs_select_4", "shortcuts.action.tabs_select_5", "shortcuts.action.tabs_select_6",
      "shortcuts.action.tabs_select_7", "shortcuts.action.tabs_select_8"};
  for (int i = 0; i < 8; ++i)
    registry.append(action(selectTab[i], "windows_tabs", selectTabLabel[i],
                           {keys(CTRL | static_cast<Qt::Key>(Qt::Key_1 + i))}));
  registry += {
      action("tabs.select_last", "windows_tabs", "shortcuts.action.tabs_select_last", {keys(CTRL | Qt::Key_9)}),
      action("tabs.duplicate", "windows_tabs", "shortcuts.action.tabs_duplicate"),
      action("tabs.search", "windows_tabs", "shortcuts.action.tabs_search", {keys(CTRL | SHIFT | Qt::Key_A)}),
      action("tabs.sleep_others", "windows_tabs", "shortcuts.action.tabs_sleep_others"),

      action("navigation.back", "navigation", "shortcuts.action.navigation_back", {keys(ALT | Qt::Key_Left)}),
      action("navigation.forward", "navigation", "shortcuts.action.navigation_forward", {keys(ALT | Qt::Key_Right)}),
      action("navigation.reload", "navigation", "shortcuts.action.navigation_reload",
             {keys(CTRL | Qt::Key_R), keys(Qt::Key_F5)}),
      action("navigation.stop", "navigation", "shortcuts.action.navigation_stop"),
      action("navigation.home", "navigation", "shortcuts.action.navigation_home", {keys(ALT | Qt::Key_Home)}),

      action("navigation.focus_address", "search_find", "shortcuts.action.navigation_focus_address",
             {keys(CTRL | Qt::Key_L)}),
      action("find.open", "search_find", "shortcuts.action.find_open", {keys(CTRL | Qt::Key_F)}),
      // F3 is the KDE binding; Ctrl+G works everywhere, as in other browsers.
      action("find.next", "search_find", "shortcuts.action.find_next", {keys(Qt::Key_F3), keys(CTRL | Qt::Key_G)}),
      action("find.previous", "search_find", "shortcuts.action.find_previous",
             {keys(SHIFT | Qt::Key_F3), keys(CTRL | SHIFT | Qt::Key_G)}),

      action("page.save", "page_operations", "shortcuts.action.page_save", {keys(CTRL | Qt::Key_S)}),
      action("page.print", "page_operations", "shortcuts.action.page_print", {keys(CTRL | Qt::Key_P)}),
      action("page.view_source", "page_operations", "shortcuts.action.page_view_source", {keys(CTRL | Qt::Key_U)}),

      action("history.open", "history_bookmarks", "shortcuts.action.history_open", {keys(CTRL | Qt::Key_H)}),
      action("bookmarks.add_current", "history_bookmarks", "shortcuts.action.bookmarks_add_current",
             {keys(CTRL | Qt::Key_D)}),
      action("bookmarks.manager", "history_bookmarks", "shortcuts.action.bookmarks_manager",
             {keys(CTRL | SHIFT | Qt::Key_O)}),
      action("bookmarks.toggle_bar", "history_bookmarks", "shortcuts.action.bookmarks_toggle_bar",
             {keys(CTRL | SHIFT | Qt::Key_B)}),
      action("downloads.open", "history_bookmarks", "shortcuts.action.downloads_open", {keys(CTRL | Qt::Key_J)}),

      action("zoom.in", "media_display", "shortcuts.action.zoom_in",
             {keys(CTRL | Qt::Key_Plus), keys(CTRL | Qt::Key_Equal)}),
      action("zoom.out", "media_display", "shortcuts.action.zoom_out", {keys(CTRL | Qt::Key_Minus)}),
      action("zoom.reset", "media_display", "shortcuts.action.zoom_reset", {keys(CTRL | Qt::Key_0)}),

      action("tabs.group_new", "dalinira", "shortcuts.action.tabs_group_new", {keys(ALT | SHIFT | Qt::Key_P)}),
      action("tabs.group_add_tab", "dalinira", "shortcuts.action.tabs_group_add_tab", {keys(ALT | SHIFT | Qt::Key_C)}),
      action("tabs.group_close", "dalinira", "shortcuts.action.tabs_group_close", {keys(ALT | SHIFT | Qt::Key_W)}),
      // Keeps auto-hiding side tabs open (or lets them hide again).
      action("sidepanel.toggle", "dalinira", "shortcuts.action.sidepanel_toggle"),
      action("settings.open", "dalinira", "shortcuts.action.settings_open"),
      action("dalinira.passwords", "dalinira", "shortcuts.action.dalinira_passwords"),
      action("dalinira.audio_effects", "dalinira", "shortcuts.action.dalinira_audio_effects"),
  };
  return registry;
}

// Keys that belong to the focused text field or to the window manager. A page
// (Chromium or WebKit), the address bar and Settings inputs keep them even
// though browser shortcuts otherwise win over page key handlers.
const QSet<QKeySequence> &reservedSequences() {
  using Qt::ALT;
  using Qt::CTRL;
  using Qt::SHIFT;
  static const QSet<QKeySequence> reserved = [] {
    QSet<QKeySequence> set;
    for (Qt::Key key : {Qt::Key_A, Qt::Key_C, Qt::Key_V, Qt::Key_X, Qt::Key_Y, Qt::Key_Z, Qt::Key_Insert,
                        Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                        Qt::Key_Down, Qt::Key_Home, Qt::Key_End})
      set.insert(keys(CTRL | key));
    for (Qt::Key key : {Qt::Key_Z, Qt::Key_V, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                        Qt::Key_Home, Qt::Key_End})
      set.insert(keys(CTRL | SHIFT | key));
    set.insert(keys(SHIFT | Qt::Key_F10));  // context menu
    set.insert(keys(ALT | Qt::Key_F4));
    set.insert(keys(ALT | Qt::Key_Tab));
    set.insert(keys(ALT | SHIFT | Qt::Key_Tab));
    return set;
  }();
  return reserved;
}

bool isModifierKey(int key) {
  switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_Mode_switch:
      return true;
    default:
      return false;
  }
}

bool isPrintableSymbol(int key) {
  return key > Qt::Key_Space && key < Qt::Key_Escape && !QChar::isLetterOrNumber(char32_t(key));
}

// One spelling per key combination: Shift+Tab arrives as Backtab; keypad or
// keyboard-group flags must not make an otherwise equal binding differ; and
// when Shift only selected a symbol (Ctrl+Shift+= types "+"), the symbol is
// the key and Shift is not part of the binding. Qt's shortcut map and the
// WebKit key path both match a typed Ctrl+Shift+= against "Ctrl++".
QKeySequence normalized(const QKeySequence &sequence) {
  if (sequence.count() != 1) return sequence;
  int key = sequence[0].key();
  Qt::KeyboardModifiers modifiers = sequence[0].keyboardModifiers()
      & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
  if (key == Qt::Key_Backtab) {
    key = Qt::Key_Tab;
    modifiers |= Qt::ShiftModifier;
  }
  if (isPrintableSymbol(key)) modifiers &= ~Qt::ShiftModifier;
  return QKeySequence(QKeyCombination(modifiers, static_cast<Qt::Key>(key)));
}

// Stable, untranslated key names: Qt's NativeText follows the desktop's Qt
// translation ("Üst Karakter" on a Turkish KDE) whatever DaliNira's language.
QString keyName(int key) {
  switch (key) {
    case Qt::Key_Left: return QStringLiteral("←");
    case Qt::Key_Right: return QStringLiteral("→");
    case Qt::Key_Up: return QStringLiteral("↑");
    case Qt::Key_Down: return QStringLiteral("↓");
    case Qt::Key_Escape: return QStringLiteral("Esc");
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return QStringLiteral("Tab");
    case Qt::Key_Space: return QStringLiteral("Space");
    case Qt::Key_Backspace: return QStringLiteral("Backspace");
    case Qt::Key_Delete: return QStringLiteral("Delete");
    case Qt::Key_Insert: return QStringLiteral("Insert");
    case Qt::Key_Home: return QStringLiteral("Home");
    case Qt::Key_End: return QStringLiteral("End");
    case Qt::Key_PageUp: return QStringLiteral("PgUp");
    case Qt::Key_PageDown: return QStringLiteral("PgDn");
    case Qt::Key_Return:
    case Qt::Key_Enter: return QStringLiteral("Enter");
    default: break;
  }
  if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
  if (key > Qt::Key_Space && key < Qt::Key_Escape) {
    const char32_t character = char32_t(key);
    return QString::fromUcs4(&character, 1).toUpper();
  }
  return QKeySequence(key).toString(QKeySequence::PortableText);
}

QList<QKeySequence> withoutDuplicates(const QList<QKeySequence> &sequences) {
  QList<QKeySequence> result;
  for (const QKeySequence &sequence : sequences)
    if (!result.contains(sequence)) result.append(sequence);
  return result;
}

}  // namespace

KeyboardShortcutService::KeyboardShortcutService(const QString &settingsFile, QObject *parent)
    : QObject(parent),
      settings_(settingsFile.isEmpty() ? std::make_unique<QSettings>()
                                       : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat)) {
  load();
}

KeyboardShortcutService::~KeyboardShortcutService() = default;

KeyboardShortcutService &KeyboardShortcutService::instance() {
  static KeyboardShortcutService service;
  return service;
}

const QList<ShortcutActionDefinition> &KeyboardShortcutService::registry() {
  static const QList<ShortcutActionDefinition> definitions = buildRegistry();
  return definitions;
}

const ShortcutActionDefinition *KeyboardShortcutService::definition(const QString &actionId) {
  for (const ShortcutActionDefinition &definition : registry())
    if (definition.id == actionId) return &definition;
  return nullptr;
}

QStringList KeyboardShortcutService::categories() {
  QStringList result;
  for (const ShortcutActionDefinition &definition : registry())
    if (!result.contains(definition.category)) result.append(definition.category);
  return result;
}

QList<QKeySequence> KeyboardShortcutService::shortcuts(const QString &actionId) const {
  return effective_.value(actionId);
}

QList<QKeySequence> KeyboardShortcutService::defaultShortcuts(const QString &actionId) const {
  const ShortcutActionDefinition *def = definition(actionId);
  return def ? def->defaults : QList<QKeySequence>{};
}

bool KeyboardShortcutService::isCustomized(const QString &actionId) const {
  const ShortcutActionDefinition *def = definition(actionId);
  return def && effective_.value(actionId) != def->defaults;
}

int KeyboardShortcutService::customizedCount() const {
  int count = 0;
  for (const ShortcutActionDefinition &definition : registry())
    if (isCustomized(definition.id)) ++count;
  return count;
}

QString KeyboardShortcutService::ownerOf(const QKeySequence &sequence, const QString &exceptActionId) const {
  const QKeySequence wanted = normalized(sequence);
  if (wanted.isEmpty()) return {};
  for (const ShortcutActionDefinition &definition : registry()) {
    if (definition.id == exceptActionId) continue;
    if (effective_.value(definition.id).contains(wanted)) return definition.id;
  }
  return {};
}

ShortcutStatus KeyboardShortcutService::validate(const QKeySequence &input) {
  if (input.isEmpty()) return ShortcutStatus::Empty;
  if (input.count() > 1) return ShortcutStatus::MultipleChords;
  const QKeySequence sequence = normalized(input);
  const int key = sequence[0].key();
  const Qt::KeyboardModifiers modifiers = sequence[0].keyboardModifiers();
  if (isModifierKey(key)) return ShortcutStatus::ModifierOnly;
  if (key == 0 || key == Qt::Key_unknown || key == Qt::Key_Multi_key
      || (key >= Qt::Key_Dead_Grave && key <= Qt::Key_Dead_Longsolidusoverlay))
    return ShortcutStatus::InvalidKey;
  if (modifiers & Qt::MetaModifier) return ShortcutStatus::MetaModifier;
  // Ctrl+Alt is AltGr on many layouts (Turkish Ctrl+Alt+Q types "@").
  if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::AltModifier)) return ShortcutStatus::AltGraph;
  const bool functionKey = key >= Qt::Key_F1 && key <= Qt::Key_F35;
  if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier)) && !functionKey)
    return ShortcutStatus::MissingModifier;
  if (reservedSequences().contains(sequence)) return ShortcutStatus::Reserved;
  return ShortcutStatus::Ok;
}

QString KeyboardShortcutService::primaryText(const QString &actionId) {
  return displayText(instance().shortcuts(actionId).value(0));
}

QKeySequence KeyboardShortcutService::canonical(const QKeySequence &sequence) { return normalized(sequence); }

QString KeyboardShortcutService::modifierText(Qt::KeyboardModifiers modifiers) {
  QStringList parts;
  if (modifiers & Qt::ControlModifier) parts << QStringLiteral("Ctrl");
  if (modifiers & Qt::MetaModifier) parts << QStringLiteral("Super");
  if (modifiers & Qt::AltModifier) parts << QStringLiteral("Alt");
  if (modifiers & Qt::ShiftModifier) parts << QStringLiteral("⇧");
  return parts.join(QLatin1Char('+'));
}

QString KeyboardShortcutService::displayText(const QKeySequence &sequence) {
  QStringList chords;
  for (int i = 0; i < sequence.count(); ++i) {
    const QKeyCombination combination = sequence[i];
    const QString modifiers = modifierText(combination.keyboardModifiers());
    const QString key = keyName(combination.key());
    chords << (modifiers.isEmpty() ? key : modifiers + QLatin1Char('+') + key);
  }
  return chords.join(QStringLiteral(", "));
}

QString KeyboardShortcutService::displayText(const QList<QKeySequence> &sequences) {
  QStringList parts;
  for (const QKeySequence &sequence : sequences) parts.append(displayText(sequence));
  return parts.join(QStringLiteral(", "));
}

ShortcutChangeResult KeyboardShortcutService::setShortcut(const QString &actionId, const QKeySequence &input,
                                                          ConflictPolicy policy) {
  if (!definition(actionId)) return {ShortcutStatus::UnknownAction, {}, {}};
  const QKeySequence sequence = normalized(input);
  const ShortcutStatus status = validate(sequence);
  if (status != ShortcutStatus::Ok) return {status, {}, sequence};
  const QString owner = ownerOf(sequence, actionId);
  if (!owner.isEmpty()) {
    if (policy == ConflictPolicy::Reject) return {ShortcutStatus::Conflict, owner, sequence};
    QList<QKeySequence> remaining = effective_.value(owner);
    remaining.removeAll(sequence);
    writeOverride(owner, remaining);
  }
  writeOverride(actionId, {sequence});
  rebuildEffective();
  emit shortcutsChanged(owner.isEmpty() ? actionId : QString());
  return {};
}

ShortcutChangeResult KeyboardShortcutService::clearShortcut(const QString &actionId) {
  if (!definition(actionId)) return {ShortcutStatus::UnknownAction, {}, {}};
  writeOverride(actionId, {});
  rebuildEffective();
  emit shortcutsChanged(actionId);
  return {};
}

ShortcutChangeResult KeyboardShortcutService::resetShortcut(const QString &actionId, ConflictPolicy policy) {
  const ShortcutActionDefinition *def = definition(actionId);
  if (!def) return {ShortcutStatus::UnknownAction, {}, {}};
  QHash<QString, QList<QKeySequence>> takenBack;
  for (const QKeySequence &sequence : def->defaults) {
    const QString owner = ownerOf(sequence, actionId);
    if (owner.isEmpty()) continue;
    if (policy == ConflictPolicy::Reject) return {ShortcutStatus::Conflict, owner, sequence};
    takenBack[owner].append(sequence);
  }
  for (auto it = takenBack.cbegin(); it != takenBack.cend(); ++it) {
    QList<QKeySequence> remaining = effective_.value(it.key());
    for (const QKeySequence &sequence : it.value()) remaining.removeAll(sequence);
    writeOverride(it.key(), remaining);
  }
  removeOverride(actionId);
  rebuildEffective();
  emit shortcutsChanged(takenBack.isEmpty() ? actionId : QString());
  return {};
}

void KeyboardShortcutService::resetAll() {
  settings_->remove(kGroup);
  settings_->sync();
  overrides_.clear();
  rebuildEffective();
  emit shortcutsChanged(QString());
}

void KeyboardShortcutService::reload() {
  settings_->sync();
  load();
  emit shortcutsChanged(QString());
}

void KeyboardShortcutService::load() {
  overrides_.clear();
  settings_->beginGroup(kGroup);
  const int version = settings_->value(kVersionKey, kStorageVersion).toInt();
  if (version > kStorageVersion)
    qWarning() << "shortcuts: stored format" << version << "is newer than" << kStorageVersion
               << "- reading the entries this version understands";
  for (const QString &id : settings_->childKeys()) {
    if (id == kVersionKey) continue;
    if (!definition(id)) {
      qWarning() << "shortcuts: ignoring unknown action" << id;
      continue;
    }
    const QVariant raw = settings_->value(id);
    // An ini value containing a comma ("Ctrl+,") can come back as a list.
    const QString text = raw.typeId() == QMetaType::QStringList ? raw.toStringList().join(QStringLiteral(", "))
                                                                 : raw.toString();
    QList<QKeySequence> sequences;
    bool valid = true;
    if (!text.trimmed().isEmpty()) {
      for (const QKeySequence &parsed : QKeySequence::listFromString(text, QKeySequence::PortableText)) {
        const QKeySequence sequence = normalized(parsed);
        if (validate(sequence) != ShortcutStatus::Ok) {
          valid = false;
          break;
        }
        sequences.append(sequence);
      }
      valid = valid && !sequences.isEmpty();
    }
    if (!valid) {
      qWarning() << "shortcuts: ignoring invalid value" << text << "for" << id << "- using the default";
      continue;
    }
    overrides_.insert(id, withoutDuplicates(sequences));
  }
  settings_->endGroup();
  rebuildEffective();
}

// Customized actions keep their keys first, then defaults fill in; a key can
// never belong to two actions, even in a hand-edited profile.
void KeyboardShortcutService::rebuildEffective() {
  effective_.clear();
  QSet<QKeySequence> used;
  const auto assign = [&](const ShortcutActionDefinition &definition, const QList<QKeySequence> &sequences) {
    QList<QKeySequence> &target = effective_[definition.id];
    for (const QKeySequence &sequence : sequences) {
      if (used.contains(sequence)) {
        qWarning() << "shortcuts:" << sequence.toString(QKeySequence::PortableText) << "is already used;"
                   << definition.id << "does not get it";
        continue;
      }
      used.insert(sequence);
      target.append(sequence);
    }
  };
  for (const ShortcutActionDefinition &definition : registry())
    if (overrides_.contains(definition.id)) assign(definition, overrides_.value(definition.id));
  for (const ShortcutActionDefinition &definition : registry())
    if (!overrides_.contains(definition.id)) assign(definition, definition.defaults);
}

void KeyboardShortcutService::writeOverride(const QString &actionId, const QList<QKeySequence> &sequences) {
  const ShortcutActionDefinition *def = definition(actionId);
  if (def && sequences == def->defaults) {
    removeOverride(actionId);
    return;
  }
  overrides_.insert(actionId, sequences);
  settings_->beginGroup(kGroup);
  settings_->setValue(kVersionKey, kStorageVersion);
  settings_->setValue(actionId, QKeySequence::listToString(sequences, QKeySequence::PortableText));
  settings_->endGroup();
  settings_->sync();
}

void KeyboardShortcutService::removeOverride(const QString &actionId) {
  overrides_.remove(actionId);
  settings_->beginGroup(kGroup);
  settings_->remove(actionId);
  settings_->endGroup();
  settings_->sync();
}

}  // namespace dalinira
