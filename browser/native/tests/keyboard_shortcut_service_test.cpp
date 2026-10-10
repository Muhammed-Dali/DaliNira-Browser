// KeyboardShortcutService: default registry, persistence, validation,
// conflicts, resets and change notifications. Bounded and display-free.
#include "core/keyboard_shortcut_service.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>

#include <cassert>
#include <iostream>

using dalinira::KeyboardShortcutService;
using dalinira::ShortcutStatus;
using Policy = KeyboardShortcutService::ConflictPolicy;

namespace {

QKeySequence seq(QKeyCombination combination) { return QKeySequence(combination); }

void writeRaw(const QString &file, const QString &key, const QVariant &value) {
  QSettings settings(file, QSettings::IniFormat);
  settings.setValue(QStringLiteral("shortcuts/") + key, value);
  settings.sync();
}

void defaultRegistry(const QString &dir) {
  const auto &registry = KeyboardShortcutService::registry();
  assert(registry.size() >= 45);
  QSet<QString> ids;
  QSet<QKeySequence> keys;
  for (const auto &definition : registry) {
    assert(!ids.contains(definition.id));
    ids.insert(definition.id);
    assert(definition.labelKey.startsWith(QStringLiteral("shortcuts.action.")));
    for (const QKeySequence &key : definition.defaults) {
      // Every default passes the same rules a user binding does, and no two
      // actions share one.
      assert(KeyboardShortcutService::validate(key) == ShortcutStatus::Ok);
      assert(!keys.contains(key));
      keys.insert(key);
    }
  }
  // The shortcuts DaliNira bound before the registry existed keep working.
  KeyboardShortcutService service(dir + QStringLiteral("/defaults.ini"));
  assert(service.shortcuts(QStringLiteral("tabs.new")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_T)});
  assert(service.shortcuts(QStringLiteral("tabs.close")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_W)});
  assert(service.shortcuts(QStringLiteral("tabs.reopen_closed")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::SHIFT | Qt::Key_T)});
  assert((service.shortcuts(QStringLiteral("navigation.reload")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_R), seq(Qt::Key_F5)}));
  assert(service.shortcuts(QStringLiteral("find.open")) == QList<QKeySequence>{QKeySequence(QKeySequence::Find)});
  assert(service.shortcuts(QStringLiteral("window.fullscreen")) == QList<QKeySequence>{seq(Qt::Key_F11)});
  assert(service.shortcuts(QStringLiteral("navigation.stop")).isEmpty());
  // Conventional browser keys, as in Zen and other browsers.
  assert((service.shortcuts(QStringLiteral("tabs.next")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_Tab), seq(Qt::CTRL | Qt::Key_PageDown)}));
  assert(service.shortcuts(QStringLiteral("tabs.select_1")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_1)});
  assert(service.shortcuts(QStringLiteral("tabs.select_8")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_8)});
  assert(service.shortcuts(QStringLiteral("tabs.select_last")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_9)});
  assert(service.customizedCount() == 0);
  // Zen's group order, DaliNira's own features last.
  assert((KeyboardShortcutService::categories() == QStringList{
      QStringLiteral("windows_tabs"), QStringLiteral("navigation"), QStringLiteral("search_find"),
      QStringLiteral("page_operations"), QStringLiteral("history_bookmarks"), QStringLiteral("media_display"),
      QStringLiteral("dalinira")}));
  std::cout << "default registry: " << registry.size() << " actions, " << keys.size() << " default keys: ok\n";
}

// Every action and category label exists in all four catalogs.
void catalogLabels() {
  for (const char *language : {"en", "tr", "fr", "ar"}) {
    QFile file(QStringLiteral(DALINIRA_SOURCE_DIR "/i18n/translations/%1.json").arg(QLatin1String(language)));
    assert(file.open(QIODevice::ReadOnly));
    const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &definition : KeyboardShortcutService::registry()) {
      assert(!catalog.value(definition.labelKey).toString().isEmpty());
      assert(!catalog.value(QStringLiteral("shortcuts.category.") + definition.category).toString().isEmpty());
    }
    assert(!catalog.value(QStringLiteral("settings.category.shortcuts")).toString().isEmpty());
  }
  std::cout << "action labels in tr/en/fr/ar: ok\n";
}

void validation() {
  const auto check = [](const QKeySequence &key, ShortcutStatus expected) {
    const ShortcutStatus actual = KeyboardShortcutService::validate(key);
    if (actual != expected)
      std::cout << "validate(" << key.toString(QKeySequence::PortableText).toStdString() << ") = "
                << static_cast<int>(actual) << ", expected " << static_cast<int>(expected) << std::endl;
    assert(actual == expected);
  };
  check(QKeySequence(), ShortcutStatus::Empty);
  check(QKeySequence(Qt::CTRL | Qt::Key_K, Qt::CTRL | Qt::Key_T), ShortcutStatus::MultipleChords);
  check(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Control), ShortcutStatus::ModifierOnly);
  check(seq(Qt::ALT | Qt::Key_Alt), ShortcutStatus::ModifierOnly);
  check(seq(Qt::Key_K), ShortcutStatus::MissingModifier);
  check(seq(Qt::SHIFT | Qt::Key_K), ShortcutStatus::MissingModifier);
  check(seq(Qt::Key_Tab), ShortcutStatus::MissingModifier);
  check(seq(Qt::META | Qt::Key_K), ShortcutStatus::MetaModifier);
  check(seq(Qt::CTRL | Qt::Key_unknown), ShortcutStatus::InvalidKey);
  check(seq(Qt::CTRL | Qt::Key_Dead_Acute), ShortcutStatus::InvalidKey);
  // Text editing stays with the focused field in pages, the address bar and
  // Settings inputs.
  for (Qt::Key key : {Qt::Key_C, Qt::Key_V, Qt::Key_X, Qt::Key_A, Qt::Key_Z, Qt::Key_Y})
    check(seq(Qt::CTRL | key), ShortcutStatus::Reserved);
  check(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), ShortcutStatus::Reserved);
  check(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_V), ShortcutStatus::Reserved);
  check(seq(Qt::SHIFT | Qt::Key_Insert), ShortcutStatus::MissingModifier);
  check(seq(Qt::SHIFT | Qt::Key_F10), ShortcutStatus::Reserved);
  check(seq(Qt::ALT | Qt::Key_F4), ShortcutStatus::Reserved);
  check(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y), ShortcutStatus::Ok);
  // Ctrl+Alt is AltGr on many layouts: it would steal typed characters.
  check(seq(Qt::CTRL | Qt::ALT | Qt::Key_Q), ShortcutStatus::AltGraph);
  check(seq(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_K), ShortcutStatus::AltGraph);
  check(seq(Qt::CTRL | Qt::ALT | Qt::Key_F2), ShortcutStatus::AltGraph);
  check(seq(Qt::Key_F2), ShortcutStatus::Ok);
  check(seq(Qt::SHIFT | Qt::Key_F6), ShortcutStatus::Ok);
  check(seq(Qt::ALT | Qt::SHIFT | Qt::Key_K), ShortcutStatus::Ok);
  // Shift+Tab arrives as Backtab; both spellings are the same binding.
  check(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), ShortcutStatus::Ok);
  std::cout << "validation (modifier-only, plain keys, Super, editing keys): ok\n";
}

void displayAndCanonicalForm() {
  using S = KeyboardShortcutService;
  // Stable names, never the desktop's translation ("Üst Karakter").
  assert(S::displayText(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)) == QString::fromUtf8("Ctrl+⇧+Y"));
  assert(S::displayText(seq(Qt::ALT | Qt::Key_Left)) == QString::fromUtf8("Alt+←"));
  assert(S::displayText(seq(Qt::ALT | Qt::Key_Right)) == QString::fromUtf8("Alt+→"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Up)) == QString::fromUtf8("Ctrl+↑"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Down)) == QString::fromUtf8("Ctrl+↓"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_PageDown)) == QStringLiteral("Ctrl+PgDn"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_PageUp)) == QStringLiteral("Ctrl+PgUp"));
  assert(S::displayText(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab)) == QString::fromUtf8("Ctrl+⇧+Tab"));
  assert(S::displayText(seq(Qt::SHIFT | Qt::Key_F3)) == QString::fromUtf8("⇧+F3"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Plus)) == QStringLiteral("Ctrl++"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Space)) == QStringLiteral("Ctrl+Space"));
  assert(S::displayText(seq(Qt::ALT | Qt::Key_Home)) == QStringLiteral("Alt+Home"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_End)) == QStringLiteral("Ctrl+End"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Backspace)) == QStringLiteral("Ctrl+Backspace"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Delete)) == QStringLiteral("Ctrl+Delete"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Escape)) == QStringLiteral("Ctrl+Esc"));
  assert(S::displayText(seq(Qt::Key_F11)) == QStringLiteral("F11"));
  assert(S::displayText(seq(Qt::CTRL | Qt::Key_Comma)) == QStringLiteral("Ctrl+,"));
  assert((S::displayText(QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_R), seq(Qt::Key_F5)}) == QStringLiteral("Ctrl+R, F5")));
  assert(S::modifierText(Qt::ControlModifier | Qt::ShiftModifier) == QString::fromUtf8("Ctrl+⇧"));
  assert(S::displayText(QKeySequence()).isEmpty());

  // When Shift only selected a symbol, the symbol is the key: Ctrl+Shift+=
  // types "+" and is the default zoom-in binding "Ctrl++". Letters, digits
  // and named keys keep Shift.
  assert(S::canonical(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus)) == seq(Qt::CTRL | Qt::Key_Plus));
  assert(S::canonical(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Underscore)) == seq(Qt::CTRL | Qt::Key_Underscore));
  assert(S::canonical(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)) == seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
  assert(S::canonical(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_1)) == seq(Qt::CTRL | Qt::SHIFT | Qt::Key_1));
  assert(S::canonical(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab)) == seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab));
  assert(S::canonical(seq(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, Qt::Key_Minus))) == seq(Qt::CTRL | Qt::Key_Minus));
  {
    QTemporaryDir dir;
    KeyboardShortcutService service(dir.filePath(QStringLiteral("canonical.ini")));
    // A shifted symbol is the same binding as its symbol: a conflict.
    const auto result = service.setShortcut(QStringLiteral("tabs.duplicate"), seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus));
    assert(result.status == ShortcutStatus::Conflict && result.conflictActionId == QStringLiteral("zoom.in"));
  }
  std::cout << "friendly display names and canonical Shift handling: ok\n";
}

void persistence(const QString &dir) {
  const QString file = dir + QStringLiteral("/persist.ini");
  {
    KeyboardShortcutService service(file);
    assert(service.setShortcut(QStringLiteral("tabs.new"), seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)).ok());
    assert(service.setShortcut(QStringLiteral("settings.open"), seq(Qt::CTRL | Qt::Key_Comma)).ok());
    assert(service.clearShortcut(QStringLiteral("window.fullscreen")).ok());
    // Setting a key back to the default stores nothing.
    assert(service.setShortcut(QStringLiteral("tabs.close"), seq(Qt::CTRL | Qt::Key_W)).ok());
    assert(!service.isCustomized(QStringLiteral("tabs.close")));
  }
  {
    QSettings raw(file, QSettings::IniFormat);
    assert(raw.value(QStringLiteral("shortcuts/version")).toInt() == 1);
    assert(raw.value(QStringLiteral("shortcuts/tabs.new")).toString() == QStringLiteral("Ctrl+Shift+Y"));
    assert(raw.contains(QStringLiteral("shortcuts/window.fullscreen")));
    assert(raw.value(QStringLiteral("shortcuts/window.fullscreen")).toString().isEmpty());
    assert(!raw.contains(QStringLiteral("shortcuts/tabs.close")));
  }
  // A restart: a new service reads the same profile.
  KeyboardShortcutService restarted(file);
  assert(restarted.shortcuts(QStringLiteral("tabs.new")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)});
  assert(restarted.shortcuts(QStringLiteral("settings.open")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_Comma)});
  assert(restarted.shortcuts(QStringLiteral("window.fullscreen")).isEmpty());
  assert(restarted.isCustomized(QStringLiteral("window.fullscreen")));
  // Missing keys use the current defaults.
  assert(restarted.shortcuts(QStringLiteral("tabs.close")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_W)});
  assert(restarted.customizedCount() == 3);
  std::cout << "save/load overrides across a restart: ok\n";
}

void invalidStoredValues(const QString &dir) {
  const QString file = dir + QStringLiteral("/invalid.ini");
  writeRaw(file, QStringLiteral("tabs.new"), QStringLiteral("this is not a key"));
  writeRaw(file, QStringLiteral("tabs.close"), QStringLiteral("Ctrl+C"));            // reserved
  writeRaw(file, QStringLiteral("window.new"), QStringLiteral("K"));                 // no modifier
  writeRaw(file, QStringLiteral("find.open"), QStringLiteral("Ctrl+K, Ctrl+F"));     // two chords
  writeRaw(file, QStringLiteral("zoom.in"), QVariant(42));
  writeRaw(file, QStringLiteral("no.such.action"), QStringLiteral("Ctrl+Shift+U"));
  writeRaw(file, QStringLiteral("navigation.reload"), QStringLiteral("Ctrl+R; F5; Ctrl+Shift+R"));
  QSettings(file, QSettings::IniFormat).setValue(QStringLiteral("shortcuts/version"), 99);

  KeyboardShortcutService service(file);  // must not crash or throw
  assert(service.shortcuts(QStringLiteral("tabs.new")) == service.defaultShortcuts(QStringLiteral("tabs.new")));
  assert(service.shortcuts(QStringLiteral("tabs.close")) == service.defaultShortcuts(QStringLiteral("tabs.close")));
  assert(service.shortcuts(QStringLiteral("window.new")) == service.defaultShortcuts(QStringLiteral("window.new")));
  assert(service.shortcuts(QStringLiteral("find.open")) == service.defaultShortcuts(QStringLiteral("find.open")));
  assert(service.shortcuts(QStringLiteral("zoom.in")) == service.defaultShortcuts(QStringLiteral("zoom.in")));
  assert(service.ownerOf(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_U)).isEmpty());
  // A valid multi-key entry from a newer format is still read.
  assert(service.shortcuts(QStringLiteral("navigation.reload")).size() == 3);

  // A hand-edited duplicate: the customized action keeps the key, the other
  // action's default yields it, so no key ever has two owners.
  const QString duplicate = dir + QStringLiteral("/duplicate.ini");
  writeRaw(duplicate, QStringLiteral("tabs.duplicate"), QStringLiteral("Ctrl+T"));
  KeyboardShortcutService resolved(duplicate);
  assert(resolved.shortcuts(QStringLiteral("tabs.duplicate")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_T)});
  assert(resolved.shortcuts(QStringLiteral("tabs.new")).isEmpty());
  assert(resolved.ownerOf(seq(Qt::CTRL | Qt::Key_T)) == QStringLiteral("tabs.duplicate"));
  std::cout << "invalid stored values fall back to defaults: ok\n";
}

void conflicts(const QString &dir) {
  KeyboardShortcutService service(dir + QStringLiteral("/conflict.ini"));
  int notifications = 0;
  QObject::connect(&service, &KeyboardShortcutService::shortcutsChanged, [&] { ++notifications; });

  const auto result = service.setShortcut(QStringLiteral("tabs.duplicate"), seq(Qt::CTRL | Qt::Key_W));
  assert(result.status == ShortcutStatus::Conflict);
  assert(result.conflictActionId == QStringLiteral("tabs.close"));
  assert(result.conflictSequence == seq(Qt::CTRL | Qt::Key_W));
  // Rejected: nothing changed and nobody was told.
  assert(service.shortcuts(QStringLiteral("tabs.duplicate")).isEmpty());
  assert(service.ownerOf(seq(Qt::CTRL | Qt::Key_W)) == QStringLiteral("tabs.close"));
  assert(notifications == 0);
  // The same key on the same action is not a conflict.
  assert(service.setShortcut(QStringLiteral("tabs.close"), seq(Qt::CTRL | Qt::Key_W)).ok());

  // Reassign after the user confirmed: the old owner loses only that key.
  assert(service.setShortcut(QStringLiteral("tabs.duplicate"), seq(Qt::CTRL | Qt::Key_W), Policy::Reassign).ok());
  assert(service.ownerOf(seq(Qt::CTRL | Qt::Key_W)) == QStringLiteral("tabs.duplicate"));
  assert(service.shortcuts(QStringLiteral("tabs.close")).isEmpty());
  assert(service.isCustomized(QStringLiteral("tabs.close")));
  assert(service.setShortcut(QStringLiteral("tabs.duplicate"), seq(Qt::Key_F5), Policy::Reassign).ok());
  assert(service.shortcuts(QStringLiteral("navigation.reload")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_R)});
  // Backtab and Shift+Tab are one binding, also for conflict detection.
  assert(service.ownerOf(seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab)) == QStringLiteral("tabs.previous"));
  // An invalid key is refused before any conflict check.
  assert(service.setShortcut(QStringLiteral("tabs.new"), seq(Qt::CTRL | Qt::Key_V)).status == ShortcutStatus::Reserved);
  assert(service.setShortcut(QStringLiteral("no.such"), seq(Qt::CTRL | Qt::Key_K)).status == ShortcutStatus::UnknownAction);
  std::cout << "conflict detection, reject and reassign: ok\n";
}

void resets(const QString &dir) {
  const QString file = dir + QStringLiteral("/reset.ini");
  KeyboardShortcutService service(file);
  QStringList changed;
  QObject::connect(&service, &KeyboardShortcutService::shortcutsChanged,
                   [&](const QString &id) { changed.append(id); });

  assert(service.setShortcut(QStringLiteral("tabs.new"), seq(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)).ok());
  assert(changed == QStringList{QStringLiteral("tabs.new")});  // runtime notification
  assert(service.resetShortcut(QStringLiteral("tabs.new")).ok());
  assert(service.shortcuts(QStringLiteral("tabs.new")) == QList<QKeySequence>{seq(Qt::CTRL | Qt::Key_T)});
  assert(!service.isCustomized(QStringLiteral("tabs.new")));
  assert(!QSettings(file, QSettings::IniFormat).contains(QStringLiteral("shortcuts/tabs.new")));

  // Resetting an action whose default another action took: refused unless
  // the user confirms, then taken back.
  assert(service.setShortcut(QStringLiteral("tabs.duplicate"), seq(Qt::CTRL | Qt::Key_T), Policy::Reassign).ok());
  assert(service.shortcuts(QStringLiteral("tabs.new")).isEmpty());
  const auto refused = service.resetShortcut(QStringLiteral("tabs.new"));
  assert(refused.status == ShortcutStatus::Conflict && refused.conflictActionId == QStringLiteral("tabs.duplicate"));
  assert(service.shortcuts(QStringLiteral("tabs.new")).isEmpty());
  assert(service.resetShortcut(QStringLiteral("tabs.new"), Policy::Reassign).ok());
  assert(service.ownerOf(seq(Qt::CTRL | Qt::Key_T)) == QStringLiteral("tabs.new"));
  assert(service.shortcuts(QStringLiteral("tabs.duplicate")).isEmpty());

  assert(service.setShortcut(QStringLiteral("zoom.reset"), seq(Qt::ALT | Qt::Key_0)).ok());
  assert(service.clearShortcut(QStringLiteral("find.open")).ok());
  assert(service.customizedCount() >= 2);
  changed.clear();
  service.resetAll();
  assert(changed == QStringList{QString()});
  assert(service.customizedCount() == 0);
  for (const auto &definition : KeyboardShortcutService::registry())
    assert(service.shortcuts(definition.id) == definition.defaults);
  assert(QSettings(file, QSettings::IniFormat).childGroups().isEmpty());
  // Reset all survives a restart.
  KeyboardShortcutService restarted(file);
  assert(restarted.customizedCount() == 0);
  std::cout << "reset one, reset with conflict, reset all, notifications: ok\n";
}

}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QTemporaryDir dir;
  assert(dir.isValid());
  defaultRegistry(dir.path());
  catalogLabels();
  validation();
  displayAndCanonicalForm();
  persistence(dir.path());
  invalidStoredValues(dir.path());
  conflicts(dir.path());
  resets(dir.path());
  std::cout << "keyboard shortcut service: all checks passed\n";
  return 0;
}
