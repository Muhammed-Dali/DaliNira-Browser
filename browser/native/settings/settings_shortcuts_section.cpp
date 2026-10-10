#include "settings_page.h"
#include "settings_ui_helpers.h"
#include "core/keyboard_shortcut_service.h"
#include "i18n/i18n.h"

#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <vector>

using namespace dalinira::settings_ui;
using dalinira::KeyboardShortcutService;
using dalinira::ShortcutStatus;

namespace {

void repolish(QWidget *widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

Qt::KeyboardModifiers modifierOf(int key) {
  switch (key) {
    case Qt::Key_Shift: return Qt::ShiftModifier;
    case Qt::Key_Control: return Qt::ControlModifier;
    case Qt::Key_Alt: return Qt::AltModifier;
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R: return Qt::MetaModifier;
    default: return Qt::NoModifier;
  }
}

// The shortcut "input" of a row, as in Zen: a quiet pill showing the current
// keys. Enter, Space or a click starts recording; while recording it takes
// every key (also the browser's own shortcuts and Tab) and records exactly one
// combination: the modifiers held when the first regular key goes down.
// Auto-repeat never records, so holding Enter or a key cannot add a second,
// accidental combination. Esc cancels; Backspace/Delete clear the shortcut.
class ShortcutCaptureButton final : public QPushButton {
 public:
  explicit ShortcutCaptureButton(QWidget *parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("settings-shortcut-edit"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QPushButton::clicked, this, [this] { beginCapture(); });
  }

  std::function<void()> started;
  std::function<void(const QKeySequence &)> captured;
  std::function<void()> cleared;

  void setIdleText(const QString &text) {
    idleText_ = text;
    if (!capturing_) setText(text);
  }

  void beginCapture() {
    if (capturing_) return;
    capturing_ = true;
    held_ = Qt::NoModifier;
    setProperty("capturing", true);
    setText(I18n::text(QStringLiteral("shortcuts.capturing")));
    repolish(this);
    setFocus(Qt::OtherFocusReason);
    if (started) started();
  }

  void cancelCapture() { endCapture(); }

  // A short green or red outline after a change, as Zen's input does.
  void flash(bool valid) {
    setProperty("flash", valid ? QStringLiteral("valid") : QStringLiteral("invalid"));
    repolish(this);
    QPointer<ShortcutCaptureButton> self(this);
    QTimer::singleShot(1200, this, [self] {
      if (!self) return;
      self->setProperty("flash", QString());
      repolish(self);
    });
  }

 protected:
  bool event(QEvent *event) override {
    if (capturing_) {
      if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
      }
      if (event->type() == QEvent::KeyPress) {
        captureKey(static_cast<QKeyEvent *>(event));
        return true;
      }
      if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (!key->isAutoRepeat()) {
          held_ &= ~modifierOf(key->key());
          showHeld();
        }
        return true;
      }
    }
    return QPushButton::event(event);
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (!event->isAutoRepeat() && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      beginCapture();
      event->accept();
      return;
    }
    QPushButton::keyPressEvent(event);
  }

  void focusOutEvent(QFocusEvent *event) override {
    cancelCapture();
    QPushButton::focusOutEvent(event);
  }

 private:
  bool endCapture() {
    if (!capturing_) return false;
    capturing_ = false;
    setProperty("capturing", false);
    setText(idleText_);
    repolish(this);
    return true;
  }

  void showHeld() {
    const QString modifiers = KeyboardShortcutService::modifierText(held_);
    setText(modifiers.isEmpty() ? I18n::text(QStringLiteral("shortcuts.capturing"))
                                : modifiers + QStringLiteral("+…"));
  }

  void captureKey(QKeyEvent *event) {
    event->accept();
    // The Enter/Space that started recording, or any held key, repeats; a
    // repeat is never a new combination.
    if (event->isAutoRepeat()) return;
    const int key = event->key();
    held_ = (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))
        | modifierOf(key);
    if (key == Qt::Key_Escape && !held_) {
      cancelCapture();
      return;
    }
    if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && !held_) {
      endCapture();
      if (cleared) cleared();
      return;
    }
    if (key == Qt::Key_AltGr || modifierOf(key) != Qt::NoModifier
        || KeyboardShortcutService::validate(QKeySequence(QKeyCombination(held_, static_cast<Qt::Key>(key))))
               == ShortcutStatus::ModifierOnly) {
      showHeld();  // keep waiting for the regular key
      return;
    }
    if (key == 0 || key == Qt::Key_unknown) return;
    Qt::KeyboardModifiers modifiers = held_;
    // A character typed with AltGr: report it as the Ctrl+Alt it stands for,
    // which validation refuses with its own message.
    if (event->modifiers() & Qt::GroupSwitchModifier) modifiers |= Qt::ControlModifier | Qt::AltModifier;
    endCapture();
    if (captured)
      captured(KeyboardShortcutService::canonical(QKeySequence(QKeyCombination(modifiers, static_cast<Qt::Key>(key)))));
  }

  QString idleText_;
  bool capturing_ = false;
  Qt::KeyboardModifiers held_ = Qt::NoModifier;
};

struct ShortcutRow {
  QString actionId;
  QLabel *title = nullptr;
  ShortcutCaptureButton *edit = nullptr;
  QPushButton *reset = nullptr;
  QLabel *message = nullptr;
};

// The reset icon in the page's theme (BrowserIcons are drawn for the dark chrome).
QIcon resetIcon(const QWidget *widget) {
  if (widget->palette().color(QPalette::Window).lightness() < 128) return BrowserIcons::icon(BrowserIcon::Reset);
  return BrowserIcons::tintedIcon(BrowserIcon::Reset, QColor(QStringLiteral("#465769")),
                                  QColor(QStringLiteral("#17212b")), QColor(QStringLiteral("#a0aab4")));
}

QString categoryLabel(const QString &category) {
  static const QHash<QString, QString> keys{
      {QStringLiteral("windows_tabs"), QStringLiteral("shortcuts.category.windows_tabs")},
      {QStringLiteral("navigation"), QStringLiteral("shortcuts.category.navigation")},
      {QStringLiteral("search_find"), QStringLiteral("shortcuts.category.search_find")},
      {QStringLiteral("page_operations"), QStringLiteral("shortcuts.category.page_operations")},
      {QStringLiteral("history_bookmarks"), QStringLiteral("shortcuts.category.history_bookmarks")},
      {QStringLiteral("media_display"), QStringLiteral("shortcuts.category.media_display")},
      {QStringLiteral("dalinira"), QStringLiteral("shortcuts.category.dalinira")},
  };
  Q_ASSERT_X(keys.contains(category), "categoryLabel", "every shortcut category needs a catalog key");
  return I18n::text(keys.value(category));
}

QString actionLabel(const QString &actionId) {
  const auto *definition = KeyboardShortcutService::definition(actionId);
  return definition ? I18n::text(definition->labelKey) : actionId;
}

QString errorText(ShortcutStatus status, const QKeySequence &sequence) {
  switch (status) {
    case ShortcutStatus::ModifierOnly: return I18n::text(QStringLiteral("shortcuts.error_modifier_only"));
    case ShortcutStatus::MissingModifier: return I18n::text(QStringLiteral("shortcuts.error_missing_modifier"));
    case ShortcutStatus::MetaModifier: return I18n::text(QStringLiteral("shortcuts.error_meta"));
    case ShortcutStatus::AltGraph: return I18n::text(QStringLiteral("shortcuts.error_altgr"));
    case ShortcutStatus::Reserved:
      return I18n::text(QStringLiteral("shortcuts.error_reserved")).arg(KeyboardShortcutService::displayText(sequence));
    default: return I18n::text(QStringLiteral("shortcuts.error_invalid"));
  }
}

void showMessage(const ShortcutRow &row, const QString &text, bool error) {
  row.message->setText(text);
  row.message->setAccessibleName(text);
  row.message->setProperty("error", error);
  repolish(row.message);
  row.message->setVisible(!text.isEmpty());
}

// DaliNira's standard confirmation: the safe choice (Cancel) is the default
// and the Escape button; nothing changes unless the user picks `acceptKey`.
bool confirm(QWidget *parent, const QString &objectName, const QString &title, const QString &text,
             const QString &acceptKey) {
  QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, parent);
  box.setObjectName(objectName);
  QPushButton *accept = box.addButton(I18n::text(acceptKey), QMessageBox::AcceptRole);
  accept->setObjectName(objectName + QStringLiteral("-accept"));
  QPushButton *cancel = box.addButton(I18n::text(QStringLiteral("common.cancel")), QMessageBox::RejectRole);
  cancel->setObjectName(objectName + QStringLiteral("-cancel"));
  box.setDefaultButton(cancel);
  box.setEscapeButton(cancel);
  box.exec();
  return box.clickedButton() == accept;
}

QString conflictText(const QString &ownerId) {
  const auto *owner = KeyboardShortcutService::definition(ownerId);
  return I18n::text(QStringLiteral("shortcuts.conflict_inline"))
      .arg(owner ? categoryLabel(owner->category) : QString(), actionLabel(ownerId));
}

}  // namespace

// Laid out like Zen's keyboard-shortcuts preferences: the page heading and
// description, one "reset all" button, then flat groups (a header over a thin
// divider) of rows "action label | shortcut pill", with no cards. A reset
// button appears beside a pill only while that shortcut differs from its
// default; problems are reported in red under the pill.
QWidget *SettingsPage::createShortcutsSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.category.shortcuts")),
                                I18n::text(QStringLiteral("settings.description.shortcuts")));
  // Zen's preferences use one readable column: here it takes the available
  // width up to 760 px (the shared section layout would otherwise give rows
  // without a width preference only half of it).
  if (QWidget *column = section.layout->parentWidget()) {
    column->setMaximumWidth(760);
    if (auto *outer = qobject_cast<QBoxLayout *>(section.page->layout())) outer->setStretchFactor(column, 100);
  }
  section.layout->setSpacing(0);
  KeyboardShortcutService &service = KeyboardShortcutService::instance();
  auto rows = std::make_shared<std::vector<ShortcutRow>>();
  auto groupTitles = std::make_shared<std::vector<std::pair<QLabel *, QString>>>();

  auto *intro = new QLabel(I18n::text(QStringLiteral("shortcuts.instructions")), section.page);
  intro->setObjectName(QStringLiteral("settings-shortcuts-intro"));
  intro->setWordWrap(true);
  section.layout->addSpacing(6);
  section.layout->addWidget(intro);
  section.layout->addSpacing(12);

  auto *resetAll = new QPushButton(I18n::text(QStringLiteral("shortcuts.reset_all_button")), section.page);
  resetAll->setObjectName(QStringLiteral("settings-shortcuts-reset-all"));
  resetAll->setIcon(resetIcon(this));
  resetAll->setAccessibleName(I18n::text(QStringLiteral("shortcuts.reset_all")));
  resetAll->setToolTip(I18n::text(QStringLiteral("shortcuts.reset_all_desc")));
  auto *resetAllRow = new QHBoxLayout;
  resetAllRow->setContentsMargins(0, 0, 0, 0);
  resetAll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  resetAllRow->addWidget(resetAll);
  auto *status = new QLabel(section.page);
  status->setObjectName(QStringLiteral("settings-shortcuts-status"));
  status->setWordWrap(true);
  status->hide();
  resetAllRow->addWidget(status, 1);
  resetAllRow->addStretch(1);
  section.layout->addLayout(resetAllRow);

  // Assigning: a key owned by another action needs the user's explicit
  // confirmation before that action loses it.
  const auto assign = [this](const ShortcutRow &row, const QKeySequence &sequence) {
    KeyboardShortcutService &service = KeyboardShortcutService::instance();
    dalinira::ShortcutChangeResult result = service.setShortcut(row.actionId, sequence);
    if (result.status == ShortcutStatus::Conflict) {
      const QString keyText = KeyboardShortcutService::displayText(result.conflictSequence);
      const QString owner = actionLabel(result.conflictActionId);
      if (!confirm(this, QStringLiteral("settings-shortcut-conflict"), I18n::text(QStringLiteral("shortcuts.conflict_title")),
                   I18n::text(QStringLiteral("shortcuts.conflict_text")).arg(keyText, owner, actionLabel(row.actionId)),
                   QStringLiteral("shortcuts.conflict_reassign"))) {
        showMessage(row, conflictText(result.conflictActionId), true);
        row.edit->flash(false);
        return;
      }
      result = service.setShortcut(row.actionId, sequence, KeyboardShortcutService::ConflictPolicy::Reassign);
      if (result.ok()) {
        showMessage(row, I18n::text(QStringLiteral("shortcuts.status_reassigned"))
                             .arg(actionLabel(row.actionId), keyText, owner), false);
        row.edit->flash(true);
        return;
      }
    }
    if (!result.ok()) {
      showMessage(row, errorText(result.status, result.conflictSequence.isEmpty() ? sequence : result.conflictSequence),
                  true);
      row.edit->flash(false);
      return;
    }
    showMessage(row, QString(), false);
    row.edit->flash(true);
  };

  const auto resetOne = [this](const ShortcutRow &row) {
    KeyboardShortcutService &service = KeyboardShortcutService::instance();
    dalinira::ShortcutChangeResult result = service.resetShortcut(row.actionId);
    if (result.status == ShortcutStatus::Conflict) {
      if (!confirm(this, QStringLiteral("settings-shortcut-reset-conflict"),
                   I18n::text(QStringLiteral("shortcuts.conflict_title")),
                   I18n::text(QStringLiteral("shortcuts.reset_conflict_text"))
                       .arg(KeyboardShortcutService::displayText(result.conflictSequence),
                            actionLabel(result.conflictActionId), actionLabel(row.actionId)),
                   QStringLiteral("shortcuts.reset_conflict_restore"))) {
        showMessage(row, conflictText(result.conflictActionId), true);
        return;
      }
      result = service.resetShortcut(row.actionId, KeyboardShortcutService::ConflictPolicy::Reassign);
    }
    if (!result.ok()) return;
    showMessage(row, QString(), false);
    row.edit->flash(true);
  };

  bool firstGroup = true;
  for (const QString &category : KeyboardShortcutService::categories()) {
    auto *header = new QLabel(categoryLabel(category), section.page);
    header->setObjectName(QStringLiteral("settings-shortcut-group"));
    header->setAccessibleName(categoryLabel(category));
    groupTitles->emplace_back(header, category);
    section.layout->addSpacing(firstGroup ? 22 : 26);
    firstGroup = false;
    section.layout->addWidget(header);
    section.layout->addSpacing(8);
    auto *divider = new QFrame(section.page);
    divider->setObjectName(QStringLiteral("settings-shortcut-divider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedHeight(1);
    section.layout->addWidget(divider);
    section.layout->addSpacing(10);

    for (const dalinira::ShortcutActionDefinition &definition : KeyboardShortcutService::registry()) {
      if (definition.category != category) continue;
      auto *row = new QWidget(section.page);
      row->setObjectName(QStringLiteral("settings-shortcut-row"));
      row->setProperty("shortcutAction", definition.id);
      auto *layout = new QHBoxLayout(row);
      layout->setContentsMargins(0, 3, 0, 3);
      layout->setSpacing(12);

      ShortcutRow entry;
      entry.actionId = definition.id;
      entry.title = new QLabel(I18n::text(definition.labelKey), row);
      entry.title->setObjectName(QStringLiteral("settings-shortcut-label"));
      entry.title->setWordWrap(true);
      // A fixed 60/40 split, as Zen's 40%-wide input: neither side grows with
      // its text (a long label or the recording prompt never moves the pill).
      entry.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      entry.title->setMinimumHeight(30);
      layout->addWidget(entry.title, 3, Qt::AlignTop);

      // The pill column: ~40% of the row, as in Zen; the reset button sits
      // just before the pill and keeps its space while hidden.
      auto *pillColumn = new QWidget(row);
      pillColumn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      pillColumn->setMinimumWidth(220);
      auto *pillLayout = new QVBoxLayout(pillColumn);
      pillLayout->setContentsMargins(0, 0, 0, 0);
      pillLayout->setSpacing(4);
      auto *pillLine = new QHBoxLayout;
      pillLine->setContentsMargins(0, 0, 0, 0);
      pillLine->setSpacing(6);
      entry.reset = new QPushButton(pillColumn);
      entry.reset->setObjectName(QStringLiteral("settings-shortcut-reset"));
      entry.reset->setProperty("shortcutAction", definition.id);
      entry.reset->setIcon(resetIcon(this));
      entry.reset->setIconSize(QSize(14, 14));
      entry.reset->setCursor(Qt::PointingHandCursor);
      QSizePolicy keepSpace = entry.reset->sizePolicy();
      keepSpace.setRetainSizeWhenHidden(true);
      entry.reset->setSizePolicy(keepSpace);
      pillLine->addWidget(entry.reset, 0, Qt::AlignVCenter);
      entry.edit = new ShortcutCaptureButton(pillColumn);
      entry.edit->setProperty("shortcutAction", definition.id);
      entry.title->setBuddy(entry.edit);
      pillLine->addWidget(entry.edit, 1);
      pillLayout->addLayout(pillLine);
      entry.message = new QLabel(pillColumn);
      entry.message->setObjectName(QStringLiteral("settings-shortcut-message"));
      entry.message->setWordWrap(true);
      entry.message->hide();
      pillLayout->addWidget(entry.message);
      layout->addWidget(pillColumn, 2, Qt::AlignTop);

      entry.edit->started = [entry] { showMessage(entry, QString(), false); };
      entry.edit->captured = [assign, entry](const QKeySequence &sequence) {
        assign(entry, sequence);
        entry.edit->setFocus(Qt::OtherFocusReason);
      };
      entry.edit->cleared = [entry] {
        KeyboardShortcutService::instance().clearShortcut(entry.actionId);
        showMessage(entry, QString(), false);
      };
      connect(entry.reset, &QPushButton::clicked, this, [resetOne, entry] {
        resetOne(entry);
        entry.edit->setFocus(Qt::OtherFocusReason);
      });
      rows->push_back(entry);
      section.layout->addWidget(row);
    }
  }

  // Everything shown comes from the service, so a change made anywhere (this
  // page, another window's Settings, a reset) shows up at once. Labels are set
  // by key: several share their text with other catalog entries, so a live
  // language switch must not guess their key from the text.
  const auto refresh = [rows, groupTitles, resetAll] {
    const KeyboardShortcutService &service = KeyboardShortcutService::instance();
    for (const auto &[header, category] : *groupTitles) {
      header->setText(categoryLabel(category));
      header->setAccessibleName(categoryLabel(category));
    }
    for (const ShortcutRow &row : *rows) {
      const QList<QKeySequence> keys = service.shortcuts(row.actionId);
      const bool customized = service.isCustomized(row.actionId);
      const QString keyText = keys.isEmpty() ? I18n::text(QStringLiteral("shortcuts.not_set"))
                                             : KeyboardShortcutService::displayText(keys);
      const QString label = actionLabel(row.actionId);
      row.title->setText(label);
      row.edit->setIdleText(keyText);
      row.edit->setProperty("customized", customized);
      row.edit->setProperty("unset", keys.isEmpty());
      repolish(row.edit);
      row.edit->setAccessibleName(I18n::text(QStringLiteral("shortcuts.edit_accessible")).arg(label, keyText));
      const QString hint = I18n::text(QStringLiteral("shortcuts.edit_description"));
      row.edit->setAccessibleDescription(
          customized ? QStringLiteral("%1. %2").arg(I18n::text(QStringLiteral("shortcuts.custom_badge")), hint) : hint);
      row.edit->setToolTip(hint);
      const QList<QKeySequence> defaults = service.defaultShortcuts(row.actionId);
      row.reset->setVisible(customized);
      row.reset->setIcon(resetIcon(row.reset));
      row.reset->setAccessibleName(I18n::text(QStringLiteral("shortcuts.reset_accessible")).arg(label));
      row.reset->setToolTip(I18n::text(QStringLiteral("shortcuts.reset_tooltip"))
                                .arg(defaults.isEmpty() ? I18n::text(QStringLiteral("shortcuts.not_set"))
                                                        : KeyboardShortcutService::displayText(defaults)));
    }
    resetAll->setEnabled(service.customizedCount() > 0);
    resetAll->setIcon(resetIcon(resetAll));
  };
  refresh();
  connect(&service, &KeyboardShortcutService::shortcutsChanged, this, [refresh] { refresh(); });
  languageRefreshers_.push_back([refresh, rows, status] {
    refresh();
    status->hide();
    for (const ShortcutRow &row : *rows) row.message->hide();
  });

  connect(resetAll, &QPushButton::clicked, this, [this, status, rows] {
    if (!confirm(this, QStringLiteral("settings-shortcuts-reset-all-confirm"),
                 I18n::text(QStringLiteral("shortcuts.reset_all_confirm_title")),
                 I18n::text(QStringLiteral("shortcuts.reset_all_confirm")),
                 QStringLiteral("shortcuts.reset_all_button"))) {
      return;
    }
    KeyboardShortcutService::instance().resetAll();
    for (const ShortcutRow &row : *rows) row.message->hide();
    status->setText(I18n::text(QStringLiteral("shortcuts.reset_all_done")));
    status->setAccessibleName(status->text());
    status->show();
  });

  section.layout->addStretch(1);
  return section.page;
}
