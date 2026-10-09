#include "tab_lifecycle_adapter.h"

#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineScript>

#include "engine/engine_view.h"

namespace dalinira {

const char *unsavedInputScript() {
  return R"JS((() => {
  const changed = (doc) => {
    for (const el of doc.querySelectorAll('input, textarea, select')) {
      if (el.disabled) continue;
      const tag = el.tagName;
      if (tag === 'TEXTAREA') {
        if (el.value !== el.defaultValue) return true;
      } else if (tag === 'SELECT') {
        const options = el.options;
        if (el.multiple) {
          for (let i = 0; i < options.length; ++i)
            if (options[i].selected !== options[i].defaultSelected) return true;
        } else {
          // Without a "selected" attribute the first option is the default.
          let initial = -1;
          for (let i = 0; i < options.length; ++i) if (options[i].defaultSelected) initial = i;
          if (initial < 0 && options.length) initial = 0;
          if (el.selectedIndex !== initial) return true;
        }
      } else {
        const type = (el.type || '').toLowerCase();
        if (type === 'checkbox' || type === 'radio') {
          if (el.checked !== el.defaultChecked) return true;
        } else if (type === 'file') {
          if (el.files && el.files.length) return true;
        } else if (!['hidden', 'submit', 'button', 'reset', 'image'].includes(type)) {
          if (el.value !== el.defaultValue) return true;
        }
      }
    }
    for (const frame of doc.querySelectorAll('iframe')) {
      try { if (frame.contentDocument && changed(frame.contentDocument)) return true; } catch (e) {}
    }
    return false;
  };
  try { return changed(document); } catch (e) { return null; }
})())JS";
}

namespace {

std::optional<bool> unsavedInputAnswer(const QVariant &value) {
  if (value.typeId() == QMetaType::Bool) return value.toBool();
  return std::nullopt;
}

TabLifecycleState fromWebEngine(QWebEnginePage::LifecycleState state) {
  switch (state) {
    case QWebEnginePage::LifecycleState::Frozen: return TabLifecycleState::Frozen;
    case QWebEnginePage::LifecycleState::Discarded: return TabLifecycleState::Discarded;
    case QWebEnginePage::LifecycleState::Active: break;
  }
  return TabLifecycleState::Active;
}

QWebEnginePage::LifecycleState toWebEngine(TabLifecycleState state) {
  switch (state) {
    case TabLifecycleState::Frozen: return QWebEnginePage::LifecycleState::Frozen;
    case TabLifecycleState::Discarded: return QWebEnginePage::LifecycleState::Discarded;
    case TabLifecycleState::Active: break;
  }
  return QWebEnginePage::LifecycleState::Active;
}

class WebEnginePageLifecycleAdapter final : public TabLifecycleAdapter {
 public:
  WebEnginePageLifecycleAdapter(QWebEnginePage *page, QObject *parent)
      : TabLifecycleAdapter(parent), page_(page) {
    connect(page, &QWebEnginePage::lifecycleStateChanged, this,
            [this](QWebEnginePage::LifecycleState state) { emit lifecycleStateChanged(fromWebEngine(state)); });
    connect(page, &QWebEnginePage::recommendedStateChanged, this,
            [this](QWebEnginePage::LifecycleState state) { emit recommendedStateChanged(fromWebEngine(state)); });
    connect(page, &QWebEnginePage::recentlyAudibleChanged, this, &TabLifecycleAdapter::audibleChanged);
    connect(page, &QWebEnginePage::loadStarted, this, &TabLifecycleAdapter::loadStarted);
    connect(page, &QWebEnginePage::loadFinished, this, &TabLifecycleAdapter::loadFinished);
    connect(page, &QWebEnginePage::urlChanged, this, &TabLifecycleAdapter::urlChanged);
  }

  TabLifecycleState lifecycleState() const override {
    return page_ ? fromWebEngine(page_->lifecycleState()) : TabLifecycleState::Active;
  }
  TabLifecycleState recommendedState() const override {
    return page_ ? fromWebEngine(page_->recommendedState()) : TabLifecycleState::Active;
  }
  bool setLifecycleState(TabLifecycleState state) override {
    if (!page_) return false;
    page_->setLifecycleState(toWebEngine(state));
    return page_->lifecycleState() == toWebEngine(state);
  }
  bool isLoading() const override { return page_ && page_->isLoading(); }
  bool isAudible() const override { return page_ && page_->recentlyAudible(); }
  QUrl url() const override { return page_ ? page_->url() : QUrl(); }
  qint64 rendererProcessId() const override { return page_ ? page_->renderProcessPid() : 0; }
  void checkUnsavedInput(std::function<void(std::optional<bool>)> done) override {
    if (!page_) return done(std::nullopt);
    // DaliNira's isolated world: reads the DOM, invisible to page scripts.
    page_->runJavaScript(QString::fromUtf8(unsavedInputScript()), QWebEngineScript::ApplicationWorld,
                         [done](const QVariant &value) { done(unsavedInputAnswer(value)); });
  }

 private:
  QPointer<QWebEnginePage> page_;
};

class EngineViewLifecycleAdapter final : public TabLifecycleAdapter {
 public:
  EngineViewLifecycleAdapter(engine::EngineView *view, QObject *parent)
      : TabLifecycleAdapter(parent), view_(view) {
    connect(view, &engine::EngineView::lifecycleStateChanged, this, &TabLifecycleAdapter::lifecycleStateChanged);
    connect(view, &engine::EngineView::audibleChanged, this, &TabLifecycleAdapter::audibleChanged);
    connect(view, &engine::EngineView::dialogStateChanged, this, &TabLifecycleAdapter::dialogStateChanged);
    connect(view, &engine::EngineView::loadStarted, this, &TabLifecycleAdapter::loadStarted);
    connect(view, &engine::EngineView::loadFinished, this, &TabLifecycleAdapter::loadFinished);
    connect(view, &engine::EngineView::urlChanged, this, &TabLifecycleAdapter::urlChanged);
  }

  TabLifecycleState lifecycleState() const override {
    return view_ ? view_->lifecycleState() : TabLifecycleState::Active;
  }
  // No engine-side recommendation: only a page on screen must stay active.
  TabLifecycleState recommendedState() const override {
    return view_ && view_->isEngineActive() ? TabLifecycleState::Active : TabLifecycleState::Discarded;
  }
  bool setLifecycleState(TabLifecycleState state) override { return view_ && view_->setLifecycleState(state); }
  bool isLoading() const override { return view_ && view_->isLoading(); }
  bool isAudible() const override { return view_ && view_->isAudible(); }
  bool hasOpenDialog() const override { return view_ && view_->hasOpenDialog(); }
  QUrl url() const override { return view_ ? view_->url() : QUrl(); }
  qint64 rendererProcessId() const override { return view_ ? view_->contentProcessId() : 0; }
  void checkUnsavedInput(std::function<void(std::optional<bool>)> done) override {
    if (!view_) return done(std::nullopt);
    view_->runJavaScript(QString::fromUtf8(unsavedInputScript()),
                         [done](const QVariant &value) { done(unsavedInputAnswer(value)); });
  }

 private:
  QPointer<engine::EngineView> view_;
};

}  // namespace

TabLifecycleAdapter *createWebEnginePageLifecycleAdapter(QWebEnginePage *page, QObject *parent) {
  return page ? new WebEnginePageLifecycleAdapter(page, parent) : nullptr;
}

TabLifecycleAdapter *createEngineViewLifecycleAdapter(engine::EngineView *view, QObject *parent) {
  return view ? new EngineViewLifecycleAdapter(view, parent) : nullptr;
}

} // namespace dalinira
