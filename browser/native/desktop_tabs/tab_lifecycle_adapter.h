#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <optional>

#include "engine/tab_lifecycle_state.h"

class QWebEnginePage;

namespace dalinira {

namespace engine {
class EngineView;
}

// One tab's page as seen by the shared memory saver (TabPerformanceManager).
// This is the only engine-specific layer of tab sleeping: policy, settings,
// exceptions and protections are shared; each engine only implements how a
// page is frozen, discarded and restored.
class TabLifecycleAdapter : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;
  ~TabLifecycleAdapter() override = default;

  virtual TabLifecycleState lifecycleState() const = 0;
  // The most aggressive state the engine itself currently allows. Chromium
  // reports its own recommendation; engines without one only object while
  // the page is on screen.
  virtual TabLifecycleState recommendedState() const = 0;
  virtual bool setLifecycleState(TabLifecycleState state) = 0;
  virtual bool isLoading() const = 0;
  virtual bool isAudible() const = 0;
  virtual bool hasOpenDialog() const { return false; }
  virtual QUrl url() const = 0;
  virtual qint64 rendererProcessId() const { return 0; }
  // Whether the page holds unsaved form input (see unsavedInputScript());
  // std::nullopt when the engine cannot tell. Runs page script, so it must be
  // asked before the page is frozen.
  virtual void checkUnsavedInput(std::function<void(std::optional<bool>)> done) { done(std::nullopt); }

 signals:
  void lifecycleStateChanged(dalinira::TabLifecycleState state);
  void recommendedStateChanged(dalinira::TabLifecycleState state);
  void audibleChanged(bool audible);
  void dialogStateChanged(bool open);
  void loadStarted();
  void loadFinished(bool ok);
  void urlChanged(const QUrl &url);
};

// The shared, read-only unsaved-input check both engines run in the page: any
// text, checkbox/radio, select or file control whose current state differs
// from the state the page loaded with (defaultValue / defaultChecked /
// defaultSelected), in the document and its same-origin frames. Rich editors
// (contenteditable, canvas) are not detected.
const char *unsavedInputScript();

// Chromium tabs (QWebEnginePage lifecycle API: Frozen and Discarded).
TabLifecycleAdapter *createWebEnginePageLifecycleAdapter(QWebEnginePage *page, QObject *parent);
// Engine-neutral EngineView tabs (WebKit).
TabLifecycleAdapter *createEngineViewLifecycleAdapter(engine::EngineView *view, QObject *parent);

} // namespace dalinira
