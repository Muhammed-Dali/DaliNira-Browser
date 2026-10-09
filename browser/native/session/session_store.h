#pragma once

#include <QString>
#include <QUrl>
#include <QVector>
#include <QUuid>
#include <QColor>
#include <optional>

class TabManager;
class QObject;
class QJsonArray;

struct SavedTab final {
  QUrl url;
  QString title;
  bool active = false;
  bool pinned = false;
  std::optional<QUuid> groupId;
  QString groupName;
  QColor groupColor;
  bool groupCollapsed = false;
  // Fully sleeping (discarded) when the session was saved.
  bool sleeping = false;
};

// Persists the open tabs of a browsing workspace.
//
// Chromium and WebKit are two independent workspaces: each engine has its own
// tab list, order, active tab, pinned/group state and sleeping state. A store
// constructed with a directory (the profile's data directory) keeps them in
//   <dir>/session/chromium/tabs.session.json
//   <dir>/session/webkit/tabs.session.json
// and `engine` selects the workspace (empty: the store's default engine).
//
// A store constructed with a path ending in ".json" is a single session file
// (tests, tools); `engine` is then ignored.
class SessionStore final {
 public:
  explicit SessionStore(QString path, QString defaultEngine = QStringLiteral("chromium"));

  static QString normalizeEngine(const QString &engine);
  // One-time move of the pre-workspace shared session (<dir>/tabs.session.json)
  // into the Chromium workspace. WebKit is never seeded from it, and an
  // existing Chromium workspace is never overwritten. The legacy file is kept
  // as tabs.session.json.migrated. Returns true when a migration happened.
  static bool migrateLegacySessionIfNeeded(const QString &baseDir);

  QString sessionFilePath(const QString &engine = QString{}) const;

  QVector<SavedTab> load(const QString &engine = QString{}) const;
  bool save(const TabManager &tabs, QObject *ownerWindow, const QString &engine = QString{}, QString *error = nullptr) const;
  bool saveSavedTabs(const QVector<SavedTab> &tabs, const QString &engine = QString{}, QString *error = nullptr) const;

 private:
  static bool writeTabs(const QString &targetPath, const QJsonArray &tabs, QString *error);

  QString path_;
  QString defaultEngine_;
};
