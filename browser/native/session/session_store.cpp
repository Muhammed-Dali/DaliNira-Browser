#include "session_store.h"

#include "tab_manager.h"
#include "tab_performance_manager.h"
#include "browser_window.h"
#include "security_utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <climits>

namespace {
// The audio runtime harness serves its disposable fixture on this endpoint.
// It must never become a user's restored browser session.
bool isRuntimeFixtureUrl(const QUrl &url) {
  const QString host = url.host().toLower();
  return (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost")) && url.port() == 18766;
}
}  // namespace

SessionStore::SessionStore(QString path, QString defaultEngine)
    : path_(std::move(path)), defaultEngine_(normalizeEngine(defaultEngine)) {}

QString SessionStore::normalizeEngine(const QString &engine) {
  const QString lower = engine.toLower().trimmed();
  if (lower == QLatin1String("webkit")) return QStringLiteral("webkit");
  return QStringLiteral("chromium");
}

bool SessionStore::migrateLegacySessionIfNeeded(const QString &baseDir) {
  if (baseDir.isEmpty()) return false;
  const QString legacyPath = baseDir + QStringLiteral("/tabs.session.json");
  const QString chromiumDir = baseDir + QStringLiteral("/session/chromium");
  const QString chromiumPath = chromiumDir + QStringLiteral("/tabs.session.json");
  if (!QFile::exists(legacyPath) || QFile::exists(chromiumPath)) return false;
  if (!QDir().mkpath(chromiumDir) || !QFile::copy(legacyPath, chromiumPath)) return false;
  QFile::setPermissions(chromiumPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  // Keep the original for a downgrade, under a name no workspace reads.
  QFile::remove(legacyPath + QStringLiteral(".migrated"));
  QFile::rename(legacyPath, legacyPath + QStringLiteral(".migrated"));
  return true;
}

QString SessionStore::sessionFilePath(const QString &engine) const {
  if (path_.endsWith(QStringLiteral(".json"))) return path_;
  const QString eng = normalizeEngine(engine.isEmpty() ? defaultEngine_ : engine);
  return path_ + QStringLiteral("/session/") + eng + QStringLiteral("/tabs.session.json");
}

QVector<SavedTab> SessionStore::load(const QString &engine) const {
  const QString targetPath = sessionFilePath(engine);
  QFile file(targetPath);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  QVector<SavedTab> result;
  QJsonArray sanitizedTabs;
  bool storageChanged = false;
  for (const QJsonValue &value : document.object().value("tabs").toArray()) {
    const QJsonObject entry = value.toObject();
    const QString originalUrl = entry.value("url").toString();
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(originalUrl));
    const QString scheme = url.scheme().toLower();
    const bool validScheme = scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
                          || scheme == QStringLiteral("dalinira");
    if (!url.isValid() || !validScheme || isRuntimeFixtureUrl(url)) {
      storageChanged = true;
      continue;
    }
    QJsonObject safeEntry = entry;
    const QString encoded = url.toString(QUrl::FullyEncoded);
    if (encoded != originalUrl) { safeEntry.insert(QStringLiteral("url"), encoded); storageChanged = true; }
    sanitizedTabs.append(safeEntry);

    std::optional<QUuid> gid;
    if (entry.contains("groupId")) {
      const QUuid parsed = QUuid::fromString(entry.value("groupId").toString());
      if (!parsed.isNull()) gid = parsed;
    }
    const QString gname = entry.value("groupName").toString();
    const QColor gcolor = entry.contains("groupColor") ? QColor(entry.value("groupColor").toString()) : QColor();
    const bool gcol = entry.value("groupCollapsed").toBool();

    result.push_back({url, entry.value("title").toString(), entry.value("active").toBool(),
                      entry.value("pinned").toBool(), gid, gname, gcolor, gcol,
                      entry.value("sleeping").toBool()});
  }
  if (storageChanged) writeTabs(targetPath, sanitizedTabs, nullptr);
  return result;
}

bool SessionStore::save(const TabManager &tabs, QObject *ownerWindow, const QString &engine, QString *error) const {
  const dalinira::TabPerformanceManager *performance = tabs.performanceManager();
  QJsonArray savedTabs;
  QVector<TabManager::TabRecord> records = tabs.recordsFor(ownerWindow);
  if (const auto *bw = qobject_cast<const BrowserWindow *>(ownerWindow)) {
    QHash<QUuid, int> visualOrder;
    const auto &visibleTabs = bw->allTabs();
    for (int i = 0; i < visibleTabs.size(); ++i) visualOrder.insert(visibleTabs[i].uuid, i);
    std::stable_sort(records.begin(), records.end(), [&visualOrder](const auto &left, const auto &right) {
      return visualOrder.value(left.id, INT_MAX) < visualOrder.value(right.id, INT_MAX);
    });
  }
  TabManager::TabId activePersistent;
  TabManager::TabId mostRecentPersistent;
  quint64 newestActivation = 0;
  for (const TabManager::TabRecord &record : records) {
    if (!record.capabilities.persistentInSession || record.detached || isRuntimeFixtureUrl(record.url)) continue;
    if (record.active) activePersistent = record.id;
    if (record.activationSerial >= newestActivation) {
      newestActivation = record.activationSerial;
      mostRecentPersistent = record.id;
    }
  }
  if (activePersistent.isNull()) activePersistent = mostRecentPersistent;
  for (const TabManager::TabRecord &record : records) {
    if (!record.capabilities.persistentInSession || record.detached || isRuntimeFixtureUrl(record.url)) continue;
    QUrl targetUrl = record.url;
    if (targetUrl.isEmpty()) {
      targetUrl = QUrl(QStringLiteral("dalinira://newtab/"));
    }
    const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(targetUrl);
    const QString scheme = persistentUrl.scheme().toLower();
    const bool validScheme = scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
                          || scheme == QStringLiteral("dalinira");
    if (!persistentUrl.isValid() || !validScheme || isRuntimeFixtureUrl(persistentUrl)) continue;

    QJsonObject tabObj{{"url", persistentUrl.toString(QUrl::FullyEncoded)}, {"title", record.title},
                        {"active", record.id == activePersistent}};
    if (record.id != activePersistent && performance && performance->isTabDiscarded(record.id)) {
      tabObj.insert(QStringLiteral("sleeping"), true);
    }
    if (ownerWindow) {
      const auto *bw = qobject_cast<const BrowserWindow*>(ownerWindow);
      if (bw) {
        for (const auto &info : bw->allTabs()) {
          if (info.uuid != record.id) continue;
          tabObj.insert(QStringLiteral("pinned"), info.isPinned);
          if (info.groupId.has_value() && !info.groupId->isNull()) {
            tabObj.insert(QStringLiteral("groupId"), info.groupId->toString());
            const auto optGroup = bw->groupForTab(info.id);
            if (optGroup.has_value()) {
              tabObj.insert(QStringLiteral("groupName"), optGroup->name);
              tabObj.insert(QStringLiteral("groupColor"), optGroup->color.name());
              tabObj.insert(QStringLiteral("groupCollapsed"), optGroup->collapsed);
            }
          }
          break;
        }
      }
    }
    savedTabs.push_back(tabObj);
  }
  return writeTabs(sessionFilePath(engine), savedTabs, error);
}

bool SessionStore::saveSavedTabs(const QVector<SavedTab> &tabs, const QString &engine, QString *error) const {
  QJsonArray savedTabs;
  for (const auto &t : tabs) {
    if (isRuntimeFixtureUrl(t.url)) continue;
    const QUrl safeUrl = BrowserSecurity::sanitizeUrlForPersistence(t.url);
    if (!safeUrl.isValid()) continue;
    QJsonObject tabObj{
        {QStringLiteral("url"), safeUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("title"), t.title},
        {QStringLiteral("active"), t.active},
        {QStringLiteral("pinned"), t.pinned}
    };
    if (t.sleeping && !t.active) tabObj.insert(QStringLiteral("sleeping"), true);
    if (t.groupId.has_value() && !t.groupId->isNull()) {
      tabObj.insert(QStringLiteral("groupId"), t.groupId->toString());
      tabObj.insert(QStringLiteral("groupName"), t.groupName);
      if (t.groupColor.isValid()) {
        tabObj.insert(QStringLiteral("groupColor"), t.groupColor.name());
      }
      tabObj.insert(QStringLiteral("groupCollapsed"), t.groupCollapsed);
    }
    savedTabs.push_back(tabObj);
  }
  return writeTabs(sessionFilePath(engine), savedTabs, error);
}

bool SessionStore::writeTabs(const QString &targetPath, const QJsonArray &tabs, QString *error) {
  QDir().mkpath(QFileInfo(targetPath).absolutePath());
  QSaveFile file(targetPath);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = QStringLiteral("Cannot write session file");
    return false;
  }
  file.write(QJsonDocument(QJsonObject{{"version", 1}, {"tabs", tabs}}).toJson(QJsonDocument::Compact));
  if (!file.commit()) {
    if (error) *error = QStringLiteral("Cannot commit session file");
    return false;
  }
  QFile::setPermissions(targetPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}
