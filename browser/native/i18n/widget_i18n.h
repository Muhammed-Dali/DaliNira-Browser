#pragma once

#include "i18n.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTabBar>
#include <QTreeWidget>
#include <QWidget>

namespace dalinira::i18n {

// Widgets that display user data (bookmark titles, page titles, file names)
// set this property so live retranslation never rewrites their text.
inline constexpr char kSkipRetranslationProperty[] = "dalinira_i18n_skip";

namespace detail {

constexpr int kItemKeyRole = Qt::UserRole + 0x4910;
constexpr int kItemAppliedRole = Qt::UserRole + 0x4911;
constexpr int kItemFormatRole = Qt::UserRole + 0x4912;
constexpr int kItemArgsRole = Qt::UserRole + 0x4913;

// A composed string: `format` is a catalog key, or a literal pattern prefixed
// with '='. Arguments prefixed with '@' are catalog keys; other arguments that
// happen to be catalog values are re-expressed in the active language too.
inline QString compose(const QString &format, const QStringList &args) {
  QString text = format.startsWith(QLatin1Char('=')) ? format.mid(1) : I18n::text(format);
  for (const QString &arg : args)
    text = text.arg(arg.startsWith(QLatin1Char('@')) ? I18n::text(arg.mid(1)) : I18n::retranslated(arg));
  return text;
}

inline QByteArray formatProperty(const char *name) { return QByteArray("_dalinira_i18n_fmt_") + name; }
inline QByteArray argsProperty(const char *name) { return QByteArray("_dalinira_i18n_args_") + name; }

// Resolves the catalog key behind a visible string and returns its text in
// the active language. The key is remembered together with the last text we
// applied; when code later changes the text, the key is resolved again.
// Strings that are not catalog values (user data, composed messages) are left
// untouched.
struct KeyState {
  QString key;
  QString applied;
  bool valid = false;
};

inline QString retranslate(const QString &current, KeyState &state) {
  if (current.isEmpty()) return current;
  if (!state.valid || current != state.applied) {
    state.key = LanguageManager::instance().keyForText(current);
    state.valid = true;
  }
  if (state.key.isEmpty()) {
    state.applied = current;
    return current;
  }
  state.applied = I18n::text(state.key);
  return state.applied;
}

inline QString retranslateProperty(QObject *object, const char *name, const QString &current) {
  const QVariant format = object->property(formatProperty(name).constData());
  if (format.isValid()) return compose(format.toString(), object->property(argsProperty(name).constData()).toStringList());
  const QByteArray keyName = QByteArray("_dalinira_i18n_key_") + name;
  const QByteArray appliedName = QByteArray("_dalinira_i18n_applied_") + name;
  const QVariant storedKey = object->property(keyName.constData());
  KeyState state{storedKey.toString(), object->property(appliedName.constData()).toString(),
                 storedKey.isValid()};
  const QString result = retranslate(current, state);
  object->setProperty(keyName.constData(), state.key);
  object->setProperty(appliedName.constData(), state.applied);
  return result;
}

template <typename Item>
inline QString retranslateItem(Item *item, int column, const QString &current) {
  const QVariant storedKey = item->data(column, kItemKeyRole);
  KeyState state{storedKey.toString(), item->data(column, kItemAppliedRole).toString(),
                 storedKey.isValid()};
  const QString result = retranslate(current, state);
  item->setData(column, kItemKeyRole, state.key);
  item->setData(column, kItemAppliedRole, state.applied);
  return result;
}

inline void retranslateAction(QAction *action) {
  if (!action || action->isSeparator() || action->property(kSkipRetranslationProperty).toBool()) return;
  const QString text = retranslateProperty(action, "text", action->text());
  if (text != action->text()) action->setText(text);
  const QString tip = retranslateProperty(action, "tooltip", action->toolTip());
  if (tip != action->toolTip()) action->setToolTip(tip);
  if (QMenu *menu = action->menu()) {
    const QString title = retranslateProperty(menu, "title", menu->title());
    if (title != menu->title()) menu->setTitle(title);
    for (QAction *child : menu->actions()) retranslateAction(child);
  }
}

}  // namespace detail

// Retranslates visible and accessibility strings of `root` and its children in
// place. Object names, settings keys, URLs and item user data are never
// touched. Composed strings (with runtime arguments) are refreshed by their
// owners, which listen to LanguageManager::languageChanged.
inline void retranslateWidgetTree(QWidget *root) {
  if (!root) return;
  QList<QWidget *> all{root};
  all.append(root->findChildren<QWidget *>(QString(), Qt::FindChildrenRecursively));
  for (QWidget *widget : std::as_const(all)) {
    if (widget->property(kSkipRetranslationProperty).toBool()) continue;
    using detail::retranslateProperty;
    const auto apply = [widget](const char *name, const QString &current, auto setter) {
      const QString next = retranslateProperty(widget, name, current);
      if (next != current) setter(next);
    };
    apply("tooltip", widget->toolTip(), [widget](const QString &v) { widget->setToolTip(v); });
    apply("accessible_name", widget->accessibleName(),
          [widget](const QString &v) { widget->setAccessibleName(v); });
    apply("accessible_description", widget->accessibleDescription(),
          [widget](const QString &v) { widget->setAccessibleDescription(v); });
    if (widget->isWindow()) {
      apply("window_title", widget->windowTitle(),
            [widget](const QString &v) { widget->setWindowTitle(v); });
    }

    if (auto *label = qobject_cast<QLabel *>(widget)) {
      apply("text", label->text(), [label](const QString &v) { label->setText(v); });
    } else if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
      apply("text", button->text(), [button](const QString &v) { button->setText(v); });
    } else if (auto *group = qobject_cast<QGroupBox *>(widget)) {
      apply("title", group->title(), [group](const QString &v) { group->setTitle(v); });
    }

    if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
      apply("placeholder", edit->placeholderText(),
            [edit](const QString &v) { edit->setPlaceholderText(v); });
    }

    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
      for (int i = 0; i < combo->count(); ++i) {
        const QByteArray name = QByteArray("combo_") + QByteArray::number(i);
        const QString current = combo->itemText(i);
        const QString next = retranslateProperty(combo, name.constData(), current);
        if (next != current) combo->setItemText(i, next);
      }
    }

    if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
      for (int i = 0; i < tabs->count(); ++i) {
        const QByteArray name = QByteArray("tab_") + QByteArray::number(i);
        const QString current = tabs->tabText(i);
        const QString next = retranslateProperty(tabs, name.constData(), current);
        if (next != current) tabs->setTabText(i, next);
      }
    }

    if (auto *list = qobject_cast<QListWidget *>(widget)) {
      for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        const QVariant format = item->data(detail::kItemFormatRole);
        if (format.isValid()) {
          const QString next = detail::compose(format.toString(), item->data(detail::kItemArgsRole).toStringList());
          if (next != item->text()) item->setText(next);
          continue;
        }
        const QVariant key = item->data(detail::kItemKeyRole);
        detail::KeyState state{key.toString(), item->data(detail::kItemAppliedRole).toString(),
                               key.isValid()};
        const QString current = item->text();
        const QString next = detail::retranslate(current, state);
        item->setData(detail::kItemKeyRole, state.key);
        item->setData(detail::kItemAppliedRole, state.applied);
        if (next != current) item->setText(next);
      }
    }

    if (auto *tree = qobject_cast<QTreeWidget *>(widget)) {
      QTreeWidgetItem *header = tree->headerItem();
      for (int column = 0; header && column < header->columnCount(); ++column) {
        const QString current = header->text(column);
        const QString next = detail::retranslateItem(header, column, current);
        if (next != current) header->setText(column, next);
      }
    }

    for (QAction *action : widget->actions()) detail::retranslateAction(action);
  }
}

// Composed strings that stay correct across a live language switch, e.g.
//   bindText(button, QStringLiteral("audio.enable_module"), {QStringLiteral("@audio.limiter")});
//   bindText(label, QStringLiteral("=%1   %2"), {symbol, QStringLiteral("@audio.module_eq32")});
inline QString bindComposed(QObject *object, const char *property, const QString &format,
                            const QStringList &args = {}) {
  object->setProperty(detail::formatProperty(property).constData(), format);
  object->setProperty(detail::argsProperty(property).constData(), args);
  return detail::compose(format, args);
}

inline void bindText(QLabel *label, const QString &format, const QStringList &args = {}) {
  label->setText(bindComposed(label, "text", format, args));
}

inline void bindText(QAbstractButton *button, const QString &format, const QStringList &args = {}) {
  button->setText(bindComposed(button, "text", format, args));
}

inline void bindToolTip(QWidget *widget, const QString &format, const QStringList &args = {}) {
  widget->setToolTip(bindComposed(widget, "tooltip", format, args));
}

inline void bindAccessibleName(QWidget *widget, const QString &format, const QStringList &args = {}) {
  widget->setAccessibleName(bindComposed(widget, "accessible_name", format, args));
}

inline void bindItemText(QListWidgetItem *item, const QString &format, const QStringList &args = {}) {
  item->setData(detail::kItemFormatRole, format);
  item->setData(detail::kItemArgsRole, args);
  item->setText(detail::compose(format, args));
}

// Refreshes every open top-level window (browser windows, Settings, internal
// pages, popups, menus) after a live language switch. Installed once.
inline void installApplicationRetranslator() {
  static bool installed = false;
  if (installed || !qApp) return;
  installed = true;
  QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, qApp, [] {
    const QList<QWidget *> windows = QApplication::topLevelWidgets();
    for (QWidget *window : windows) retranslateWidgetTree(window);
  });
}

}  // namespace dalinira::i18n
