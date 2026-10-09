#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QUuid>
#include <QWebEngineView>
#include <memory>

#include "desktop_tabs/tab_resource_model.h"

class QWebEnginePage;
class QWidget;

namespace dalinira {
class TabPerformanceManager;
class TabLifecycleAdapter;
}

class TabManager final : public QObject {
  Q_OBJECT

 public:
  using TabId = QUuid;

  enum class TabKind { Web, Internal };

  struct TabCapabilities {
    bool closable = true;
    bool reorderable = true;
    bool detachable = true;
    bool persistentInSession = true;
  };

  struct TabRecord {
    TabId id;
    TabKind kind = TabKind::Web;
    QPointer<QWidget> content;
    QPointer<QWebEngineView> view;
    QPointer<QWebEnginePage> page;
    // The page's lifecycle, independent of its engine (memory saver / sleeping).
    QPointer<dalinira::TabLifecycleAdapter> lifecycle;
    QPointer<QObject> ownerWindow;
    QString title;
    QString internalId;
    QUrl url;
    QIcon icon;
    TabCapabilities capabilities;
    bool active = false;
    bool detached = false;
    bool recentlyAudible = false;
    bool rendererCrashed = false;
    int rendererExitCode = 0;
    QString rendererStatus;
    int order = 0;
    quint64 activationSerial = 0;
  };

  explicit TabManager(QObject *parent = nullptr);
  ~TabManager() override;

  TabId registerTab(QWebEngineView *view, QObject *ownerWindow, bool detached, const QString &title);
  TabId registerEngineTab(QWidget *content, QObject *ownerWindow, bool detached, const QString &title, const QUrl &url = {});
  TabId registerInternalTab(QWidget *content, QObject *ownerWindow, const QString &title,
                            const QString &internalId, TabCapabilities capabilities);
  bool transfer(TabId id, QObject *ownerWindow, bool detached);
  bool activate(TabId id);
  bool remove(TabId id);
  bool updateTitle(TabId id, const QString &title);
  bool updateUrl(TabId id, const QUrl &url);
  bool updateIcon(TabId id, const QIcon &icon);
  bool updateRecentlyAudible(TabId id, bool audible);
  bool markRendererCrashed(TabId id, int terminationStatus, int exitCode);
  bool reorder(QObject *ownerWindow, const QVector<TabId> &orderedIds);

  TabId idFor(const QWebEngineView *view) const;
  TabId idForPage(const QWebEnginePage *page) const;
  TabId idForContent(const QWidget *content) const;
  TabId findInternal(QObject *ownerWindow, const QString &internalId) const;
  TabId activeFor(QObject *ownerWindow) const;
  const TabRecord *record(TabId id) const;
  QVector<TabRecord> recordsFor(QObject *ownerWindow) const;
  // Every tab of every window.
  QVector<TabId> allIds() const { return records_.keys().toVector(); }
  int recordCount() const { return records_.size(); }
  bool validate(QString *reason = nullptr) const;

  dalinira::TabPerformanceManager *performanceManager() const;
  // The shared resource model (lifecycle, memory, CPU) of one tab, for either
  // engine; see desktop_tabs/tab_resource_model.h.
  dalinira::TabResourceSnapshot resourceSnapshot(TabId id);

 signals:
  void tabRegistered(TabManager::TabId id, TabManager::TabKind kind);
  void tabActivated(TabManager::TabId id);
  void tabTransferred(TabManager::TabId id, QObject *newOwner, bool detached);
  void tabRemoved(TabManager::TabId id);
  void tabUrlChanged(TabManager::TabId id, const QUrl &url);
  void tabRecentlyAudibleChanged(TabManager::TabId id, bool audible);

 private:
  QHash<TabId, TabRecord> records_;
  QHash<const QWebEngineView *, TabId> viewIndex_;
  QHash<const QWidget *, TabId> contentIndex_;
  quint64 activationSerial_ = 0;
  std::unique_ptr<dalinira::TabPerformanceManager> performanceManager_;
  std::unique_ptr<dalinira::TabResourceModel> resourceModel_;
};
