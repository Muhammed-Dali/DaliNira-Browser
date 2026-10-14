#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace dalinira::engine {

class GeckoBackend;

// DaliNira's browser shortcuts while a Gecko page has the keyboard. The page
// lives in another process with its own X11 window, so its keys never reach
// Qt (or the window's QShortcuts). The host keeps the keys of DaliNira's
// shortcuts from its pages instead and reports them; this router gives it
// the effective bindings (KeyboardShortcutService, again whenever they
// change) and activates the matching QShortcut of the window that shows the
// page, as WebKit's key path does. Every other key stays the page's.
//
// A report is acted on only for the host's active tab while a visible view
// of its window shows it; reports for any other tab, a stale binding or a
// lost session are dropped.
class GeckoShortcutRouter : public QObject {
  Q_OBJECT

 public:
  explicit GeckoShortcutRouter(GeckoBackend *backend);

  // The host's name of a shortcut ("Ctrl+Shift+Tab", "Alt+ArrowLeft",
  // "Ctrl++"); empty when the host cannot report it (e.g. a non-ASCII key).
  static QString hostShortcut(const QKeySequence &sequence);
  // What the host was last given.
  QStringList hostShortcuts() const { return sequences_.keys(); }

 signals:
  // The host acknowledged the bindings it was last given.
  void shortcutsPublished();

 private:
  void publish();
  void dispatch(const QString &tabId, const QString &shortcut, bool repeat);

  QPointer<GeckoBackend> backend_;
  QHash<QString, QKeySequence> sequences_;  // host name → binding
  quint64 publication_ = 0;
};

}  // namespace dalinira::engine
