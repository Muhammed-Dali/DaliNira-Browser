#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace dalinira {

// One user-visible browser action that can carry keyboard shortcuts. The id is
// stable (persisted, used by tests); labels are catalog keys resolved by the UI.
struct ShortcutActionDefinition {
  QString id;            // "tabs.new"
  QString category;      // "tabs"; its label is "shortcuts.category.<category>"
  QString labelKey;      // "shortcuts.action.tabs_new"
  QList<QKeySequence> defaults;
};

enum class ShortcutStatus {
  Ok,
  Empty,
  MultipleChords,    // "Ctrl+K, Ctrl+T": only single key combinations
  ModifierOnly,      // Ctrl+Shift with no regular key
  MissingModifier,   // a plain or Shift-only key would break typing
  MetaModifier,      // Super/Meta belongs to the desktop (and WebKit drops it)
  AltGraph,          // Ctrl+Alt types characters (AltGr) on many keyboard layouts
  Reserved,          // text editing and window-manager keys stay with the focus widget
  InvalidKey,
  UnknownAction,
  Conflict,          // owned by conflictActionId; nothing was changed
};

struct ShortcutChangeResult {
  ShortcutStatus status = ShortcutStatus::Ok;
  QString conflictActionId;
  QKeySequence conflictSequence;
  bool ok() const { return status == ShortcutStatus::Ok; }
};

// The single authority for DaliNira's keyboard shortcuts, shared by both
// engines: the default registry, the user's overrides (QSettings group
// "shortcuts", one entry per customized action) and conflict rules. Browser
// windows bind one QShortcut per action from it and rebind in place when
// shortcutsChanged() fires; the Settings page edits it.
//
// Stored format (PortableText):  [shortcuts] version=1, tabs.new=Ctrl+Shift+Y,
// navigation.reload=Ctrl+R; F5, window.close=   (empty: deliberately unset).
// A missing entry means "use the current default". Unknown ids and invalid
// values are ignored (the default applies), so a damaged profile never blocks
// startup.
class KeyboardShortcutService final : public QObject {
  Q_OBJECT
 public:
  enum class ConflictPolicy { Reject, Reassign };

  // `settingsFile` empty: the application's normal settings (the profile).
  explicit KeyboardShortcutService(const QString &settingsFile = {}, QObject *parent = nullptr);
  ~KeyboardShortcutService() override;

  static KeyboardShortcutService &instance();
  static const QList<ShortcutActionDefinition> &registry();
  static const ShortcutActionDefinition *definition(const QString &actionId);
  static QStringList categories();

  QList<QKeySequence> shortcuts(const QString &actionId) const;
  QList<QKeySequence> defaultShortcuts(const QString &actionId) const;
  bool isCustomized(const QString &actionId) const;
  int customizedCount() const;
  // The action that currently owns `sequence`, other than `exceptActionId`.
  QString ownerOf(const QKeySequence &sequence, const QString &exceptActionId = {}) const;

  static ShortcutStatus validate(const QKeySequence &sequence);
  // Stable display text for every shortcut UI (Settings, menus, tooltips):
  // "Ctrl+⇧+T, F5", "Alt+←", "Ctrl+PgDn"; never translated by the desktop.
  static QString displayText(const QList<QKeySequence> &sequences);
  static QString displayText(const QKeySequence &sequence);
  // The action's first shortcut as display text, empty without one.
  static QString primaryText(const QString &actionId);
  // Held modifiers alone, as displayText names them ("Ctrl+⇧").
  static QString modifierText(Qt::KeyboardModifiers modifiers);
  // The one spelling a combination is stored and compared in.
  static QKeySequence canonical(const QKeySequence &sequence);

  // Makes `sequence` the action's only shortcut. With Reject, a sequence owned
  // by another action is refused (status Conflict, nothing changes); with
  // Reassign, the other action loses that sequence.
  ShortcutChangeResult setShortcut(const QString &actionId, const QKeySequence &sequence,
                                   ConflictPolicy policy = ConflictPolicy::Reject);
  // Leaves the action without a shortcut.
  ShortcutChangeResult clearShortcut(const QString &actionId);
  // Restores the action's defaults; a default now owned by another action is a
  // Conflict under Reject, and is taken back from that action under Reassign.
  ShortcutChangeResult resetShortcut(const QString &actionId,
                                     ConflictPolicy policy = ConflictPolicy::Reject);
  void resetAll();
  // Re-reads the stored overrides (another settings writer, tests).
  void reload();

 signals:
  // `actionId` empty: any number of actions changed.
  void shortcutsChanged(const QString &actionId);

 private:
  void load();
  void rebuildEffective();
  void writeOverride(const QString &actionId, const QList<QKeySequence> &sequences);
  void removeOverride(const QString &actionId);

  std::unique_ptr<QSettings> settings_;
  QHash<QString, QList<QKeySequence>> overrides_;
  QHash<QString, QList<QKeySequence>> effective_;
};

}  // namespace dalinira
