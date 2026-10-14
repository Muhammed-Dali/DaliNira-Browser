#include "engine/gecko/gecko_shortcut_router.h"

#include <QLoggingCategory>
#include <QShortcut>
#include <QWidget>

#include <algorithm>

#include "core/keyboard_shortcut_service.h"
#include "engine/gecko/gecko_backend.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_ipc_server.h"
#include "engine/gecko/gecko_session.h"

Q_LOGGING_CATEGORY(lcGeckoShortcuts, "dalinira.gecko.shortcuts")

namespace dalinira::engine {

namespace {

QString hostKeyName(int key) {
  switch (key) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return QStringLiteral("Tab");
    case Qt::Key_Return:
    case Qt::Key_Enter: return QStringLiteral("Enter");
    case Qt::Key_Escape: return QStringLiteral("Escape");
    case Qt::Key_Backspace: return QStringLiteral("Backspace");
    case Qt::Key_Delete: return QStringLiteral("Delete");
    case Qt::Key_Insert: return QStringLiteral("Insert");
    case Qt::Key_Home: return QStringLiteral("Home");
    case Qt::Key_End: return QStringLiteral("End");
    case Qt::Key_PageUp: return QStringLiteral("PageUp");
    case Qt::Key_PageDown: return QStringLiteral("PageDown");
    case Qt::Key_Left: return QStringLiteral("ArrowLeft");
    case Qt::Key_Right: return QStringLiteral("ArrowRight");
    case Qt::Key_Up: return QStringLiteral("ArrowUp");
    case Qt::Key_Down: return QStringLiteral("ArrowDown");
    case Qt::Key_Space: return QStringLiteral("Space");
    default: break;
  }
  if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
  // Qt's keys for printable ASCII are the (upper-case) characters themselves.
  if (key >= Qt::Key_Exclam && key <= Qt::Key_AsciiTilde) return QString(QChar(key));
  return {};
}

}  // namespace

GeckoShortcutRouter::GeckoShortcutRouter(GeckoBackend *backend) : QObject(backend), backend_(backend) {
  GeckoSession *session = backend->session();
  connect(session, &GeckoSession::ready, this, &GeckoShortcutRouter::publish);
  // The host is gone (crashed, or stopped): nothing of its set is acted on.
  connect(session, &GeckoSession::disconnected, this, [this] { sequences_.clear(); });
  connect(session, &GeckoSession::hostExited, this, [this] { sequences_.clear(); });
  connect(session, &GeckoSession::shortcutPressed, this, &GeckoShortcutRouter::dispatch);
  connect(&KeyboardShortcutService::instance(), &KeyboardShortcutService::shortcutsChanged, this,
          &GeckoShortcutRouter::publish);
}

QString GeckoShortcutRouter::hostShortcut(const QKeySequence &sequence) {
  if (sequence.count() != 1) return {};
  const QKeyCombination combination = KeyboardShortcutService::canonical(sequence)[0];
  const QString key = hostKeyName(combination.key());
  if (key.isEmpty()) return {};
  const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
  QString name;
  if (modifiers & Qt::ControlModifier) name += QStringLiteral("Ctrl+");
  if (modifiers & Qt::AltModifier) name += QStringLiteral("Alt+");
  if (modifiers & Qt::ShiftModifier) name += QStringLiteral("Shift+");
  name += key;
  // Meta, AltGr-like and reserved combinations are never the host's to keep.
  return !(modifiers & Qt::MetaModifier) && GeckoIpcServer::isValidShortcut(name) ? name : QString();
}

void GeckoShortcutRouter::publish() {
  if (!backend_ || backend_->session()->state() != GeckoSession::State::Ready) return;
  const KeyboardShortcutService &service = KeyboardShortcutService::instance();
  QHash<QString, QKeySequence> sequences;
  for (const ShortcutActionDefinition &definition : KeyboardShortcutService::registry()) {
    for (const QKeySequence &sequence : service.shortcuts(definition.id)) {
      const QString name = hostShortcut(sequence);
      if (!name.isEmpty()) sequences.insert(name, sequence);
    }
  }
  // At once: a report of a binding that just went away is dropped.
  sequences_ = sequences;
  QStringList names = sequences.keys();
  std::sort(names.begin(), names.end());
  const quint64 publication = ++publication_;
  GeckoRequest *request = backend_->session()->setShortcuts(names);
  connect(request, &GeckoRequest::finished, this, [this, publication](const QString &status, const QJsonObject &result) {
    if (status != QLatin1String("completed")) {
      qCWarning(lcGeckoShortcuts) << "the Gecko host did not take the browser shortcuts:" << status << result;
    } else if (publication == publication_) {
      emit shortcutsPublished();
    }
  });
}

void GeckoShortcutRouter::dispatch(const QString &tabId, const QString &shortcut, bool repeat) {
  const QKeySequence sequence = sequences_.value(shortcut);
  GeckoEngineView *view = backend_ ? backend_->viewShowing(tabId) : nullptr;
  if (sequence.isEmpty() || !view || backend_->session()->activeTabId() != tabId || !view->isEngineActive() ||
      !view->isVisible() || view->window()->isMinimized()) {
    qCDebug(lcGeckoShortcuts) << "dropped" << shortcut << "of tab" << tabId;
    return;
  }
  const auto shortcuts = view->window()->findChildren<QShortcut *>();
  for (QShortcut *candidate : shortcuts) {
    if (!candidate->isEnabled() || !candidate->keys().contains(sequence)) continue;
    if (candidate->context() != Qt::WindowShortcut && candidate->context() != Qt::ApplicationShortcut) continue;
    auto *owner = qobject_cast<QWidget *>(candidate->parent());
    if (owner && !owner->isVisible()) continue;
    if (repeat && !candidate->autoRepeat()) return;
    qCDebug(lcGeckoShortcuts) << shortcut << "of tab" << tabId << "runs" << candidate->objectName();
    // Queued, as for WebKit: the action may close this very view.
    QMetaObject::invokeMethod(candidate, "activated", Qt::QueuedConnection);
    return;
  }
}

}  // namespace dalinira::engine
