#include "tab_manager.h"
#include "tab_performance_manager.h"
#include "tab_lifecycle_adapter.h"
#include "engine/engine_view.h"

#include <QSet>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>

#include <algorithm>

TabManager::TabManager(QObject *parent)
    : QObject(parent),
      performanceManager_(std::make_unique<dalinira::TabPerformanceManager>(this, nullptr)),
      resourceModel_(std::make_unique<dalinira::TabResourceModel>()) {
  connect(performanceManager_.get(), &dalinira::TabPerformanceManager::tabAboutToSleep, this,
          [this](TabId id) { resourceModel_->noteAboutToSleep(id, dalinira::TabResourceModel::inputs(*this)); });
}

TabManager::~TabManager() = default;

TabManager::TabId TabManager::registerTab(QWebEngineView *view, QObject *ownerWindow, bool detached, const QString &title) {
  if (!view || !view->page() || !ownerWindow) return {};
  if (const TabId known = idFor(view); !known.isNull()) return known;

  int nextOrder = 0;
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow) nextOrder = std::max(nextOrder, record.order + 1);
  }
  TabRecord record;
  record.id = QUuid::createUuid();
  record.kind = TabKind::Web;
  record.content = view;
  record.view = view;
  record.page = view->page();
  record.lifecycle = dalinira::createWebEnginePageLifecycleAdapter(view->page(), this);
  record.ownerWindow = ownerWindow;
  record.title = title;
  record.url = view->url();
  record.detached = detached;
  record.capabilities = {true, true, true, true};
  record.order = nextOrder;
  records_.insert(record.id, record);
  viewIndex_.insert(view, record.id);
  contentIndex_.insert(view, record.id);
  connect(view, &QObject::destroyed, this, [this, id = record.id] { remove(id); });
  emit tabRegistered(record.id, TabKind::Web);
  return record.id;
}

TabManager::TabId TabManager::registerEngineTab(QWidget *content, QObject *ownerWindow, bool detached,
                                                const QString &title, const QUrl &url) {
  if (!content || !ownerWindow) return {};
  if (const TabId known = idForContent(content); !known.isNull()) return known;

  int nextOrder = 0;
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow) nextOrder = std::max(nextOrder, record.order + 1);
  }
  TabRecord record;
  record.id = QUuid::createUuid();
  record.kind = TabKind::Web;
  record.content = content;
  record.lifecycle = dalinira::createEngineViewLifecycleAdapter(qobject_cast<dalinira::engine::EngineView *>(content), this);
  record.ownerWindow = ownerWindow;
  record.title = title;
  record.url = url;
  record.detached = detached;
  record.capabilities = {true, true, true, true};
  record.order = nextOrder;
  records_.insert(record.id, record);
  contentIndex_.insert(content, record.id);
  connect(content, &QObject::destroyed, this, [this, id = record.id] { remove(id); });
  emit tabRegistered(record.id, TabKind::Web);
  return record.id;
}

TabManager::TabId TabManager::registerInternalTab(QWidget *content, QObject *ownerWindow,
                                                   const QString &title, const QString &internalId,
                                                   TabCapabilities capabilities) {
  if (!content || !ownerWindow || internalId.trimmed().isEmpty()) return {};
  if (const TabId known = idForContent(content); !known.isNull()) return known;
  if (!findInternal(ownerWindow, internalId).isNull()) return {};
  int nextOrder = 0;
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow) nextOrder = std::max(nextOrder, record.order + 1);
  }
  TabRecord record;
  record.id = QUuid::createUuid();
  record.kind = TabKind::Internal;
  record.content = content;
  record.ownerWindow = ownerWindow;
  record.title = title;
  record.internalId = internalId;
  record.capabilities = capabilities;
  record.detached = false;
  record.order = nextOrder;
  records_.insert(record.id, record);
  contentIndex_.insert(content, record.id);
  connect(content, &QObject::destroyed, this, [this, id = record.id] { remove(id); });
  emit tabRegistered(record.id, TabKind::Internal);
  return record.id;
}

bool TabManager::transfer(TabId id, QObject *ownerWindow, bool detached) {
  auto it = records_.find(id);
  // Any registered tab moves: Chromium pages, engine (WebKit) pages and
  // internal pages alike.
  if (it == records_.end() || !ownerWindow || !it->content) return false;
  if (detached && !it->capabilities.detachable) return false;
  for (auto other = records_.begin(); other != records_.end(); ++other) {
    if (other->ownerWindow == ownerWindow) other->active = false;
  }
  it->ownerWindow = ownerWindow;
  it->detached = detached;
  it->active = true;
  it->activationSerial = ++activationSerial_;
  int nextOrder = 0;
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow && record.id != id) nextOrder = std::max(nextOrder, record.order + 1);
  }
  it->order = nextOrder;
  emit tabTransferred(id, ownerWindow, detached);
  return true;
}

bool TabManager::activate(TabId id) {
  auto it = records_.find(id);
  if (it == records_.end() || !it->ownerWindow) return false;
  for (auto other = records_.begin(); other != records_.end(); ++other) {
    if (other->ownerWindow == it->ownerWindow) other->active = false;
  }
  it->active = true;
  it->activationSerial = ++activationSerial_;
  emit tabActivated(id);
  return true;
}

bool TabManager::remove(TabId id) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  const TabId removedId = id;
  viewIndex_.remove(it->view);
  contentIndex_.remove(it->content);
  QPointer<dalinira::TabLifecycleAdapter> lifecycle = it->lifecycle;
  records_.erase(it);
  emit tabRemoved(removedId);
  delete lifecycle.data();
  return true;
}

bool TabManager::updateTitle(TabId id, const QString &title) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  it->title = title;
  return true;
}

bool TabManager::updateUrl(TabId id, const QUrl &url) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  it->url = url;
  emit tabUrlChanged(id, url);
  return true;
}

bool TabManager::updateIcon(TabId id, const QIcon &icon) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  if (icon.isNull() && !it->icon.isNull()) {
    // Retain existing valid favicon if a null icon signal is received.
    return true;
  }
  it->icon = icon;
  return true;
}

bool TabManager::updateRecentlyAudible(TabId id, bool audible) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  if (it->recentlyAudible == audible) return true;
  it->recentlyAudible = audible;
  emit tabRecentlyAudibleChanged(id, audible);
  return true;
}

bool TabManager::markRendererCrashed(TabId id, int terminationStatus, int exitCode) {
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  it->rendererCrashed = true;
  it->rendererExitCode = exitCode;
  it->rendererStatus = QString::number(terminationStatus);
  return true;
}

bool TabManager::reorder(QObject *ownerWindow, const QVector<TabId> &orderedIds) {
  if (!ownerWindow) return false;
  QSet<TabId> seen;
  for (const TabId &id : orderedIds) {
    const auto it = records_.constFind(id);
    if (it == records_.cend() || it->ownerWindow != ownerWindow || seen.contains(id)) return false;
    seen.insert(id);
  }
  const QVector<TabRecord> owned = recordsFor(ownerWindow);
  if (owned.size() != orderedIds.size()) return false;
  for (int index = 0; index < orderedIds.size(); ++index) records_[orderedIds[index]].order = index;
  return true;
}

TabManager::TabId TabManager::idFor(const QWebEngineView *view) const {
  return viewIndex_.value(view, {});
}

TabManager::TabId TabManager::idForPage(const QWebEnginePage *page) const {
  if (!page) return {};
  for (const TabRecord &record : records_) {
    if (record.page == page) return record.id;
  }
  return {};
}

TabManager::TabId TabManager::idForContent(const QWidget *content) const {
  return contentIndex_.value(content, {});
}

TabManager::TabId TabManager::findInternal(QObject *ownerWindow, const QString &internalId) const {
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow && record.kind == TabKind::Internal && record.internalId == internalId)
      return record.id;
  }
  return {};
}

TabManager::TabId TabManager::activeFor(QObject *ownerWindow) const {
  for (const TabRecord &record : records_) {
    if (record.ownerWindow == ownerWindow && record.active) return record.id;
  }
  return {};
}

const TabManager::TabRecord *TabManager::record(TabId id) const {
  const auto it = records_.constFind(id);
  return it == records_.cend() ? nullptr : &it.value();
}

QVector<TabManager::TabRecord> TabManager::recordsFor(QObject *ownerWindow) const {
  QVector<TabRecord> result;
  for (const TabRecord &record : records_) if (record.ownerWindow == ownerWindow) result.push_back(record);
  std::sort(result.begin(), result.end(), [](const TabRecord &left, const TabRecord &right) { return left.order < right.order; });
  return result;
}

bool TabManager::validate(QString *reason) const {
  QSet<const QWidget *> contents;
  QSet<const QWebEnginePage *> pages;
  QHash<const QObject *, int> activeByOwner;
  for (const TabRecord &record : records_) {
    if (!record.content || !record.ownerWindow) {
      if (reason) *reason = "tab record has missing content or owner";
      return false;
    }
    if (contents.contains(record.content)) {
      if (reason) *reason = "one live content widget belongs to more than one tab record";
      return false;
    }
    contents.insert(record.content);
    // Chromium tabs carry their QWebEngineView/QWebEnginePage; other engines'
    // tabs are the engine view itself and have only a lifecycle adapter.
    const bool engineTab = record.kind == TabKind::Web && !record.view && !record.page;
    if (engineTab && !record.lifecycle) {
      if (reason) *reason = "engine tab has no lifecycle";
      return false;
    }
    if (record.kind == TabKind::Web && !engineTab && (!record.view || !record.page || record.view != record.content)) {
      if (reason) *reason = "web tab has missing or mismatched view/page";
      return false;
    }
    if (record.kind == TabKind::Internal && (record.view || record.page || record.internalId.isEmpty())) {
      if (reason) *reason = "internal tab has web state or missing identity";
      return false;
    }
    if (record.kind == TabKind::Web && !engineTab && record.view->page() != record.page) {
      if (reason) *reason = "tab record page does not match the live view page";
      return false;
    }
    if (record.page && pages.contains(record.page)) {
      if (reason) *reason = "one live page belongs to more than one tab record";
      return false;
    }
    if (record.page) pages.insert(record.page);
    if (record.active) ++activeByOwner[record.ownerWindow];
  }
  for (auto it = activeByOwner.cbegin(); it != activeByOwner.cend(); ++it) {
    if (it.value() > 1) {
      if (reason) *reason = "a native window has more than one active tab";
      return false;
    }
  }
  return true;
}

dalinira::TabResourceSnapshot TabManager::resourceSnapshot(TabId id) {
  return resourceModel_->snapshot(id, dalinira::TabResourceModel::inputs(*this));
}

dalinira::TabPerformanceManager *TabManager::performanceManager() const {
  return performanceManager_.get();
}
