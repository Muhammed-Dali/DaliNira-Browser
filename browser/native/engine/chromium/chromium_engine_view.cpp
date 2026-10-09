#include <QWebEngineScript>
#include "engine/chromium/chromium_engine_view.h"
#include <QVBoxLayout>
#include <QWebEngineHistory>
#include <QWebEngineView>
#include <QWebEngineFindTextResult>

namespace dalinira::engine {

ChromiumEngineView::ChromiumEngineView(QWebEngineView *webView, QWidget *parent)
    : EngineView(parent), webView_(webView) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  if (webView_) {
    layout->addWidget(webView_);
    connect(webView_, &QWebEngineView::urlChanged, this,
            &EngineView::urlChanged);
    connect(webView_, &QWebEngineView::titleChanged, this,
            &EngineView::titleChanged);
    connect(webView_, &QWebEngineView::iconChanged, this,
            &EngineView::iconChanged);
    connect(webView_, &QWebEngineView::loadStarted, this, [this]() {
      isLoading_ = true;
      emit loadStarted();
    });
    connect(webView_, &QWebEngineView::loadProgress, this,
            &EngineView::loadProgress);
    connect(webView_, &QWebEngineView::loadFinished, this, [this](bool ok) {
      isLoading_ = false;
      emit loadFinished(ok);
    });
  }
}

ChromiumEngineView::~ChromiumEngineView() = default;

QUrl ChromiumEngineView::url() const {
  return webView_ ? webView_->url() : QUrl{};
}

QString ChromiumEngineView::title() const {
  return webView_ ? webView_->title() : QString{};
}

bool ChromiumEngineView::isLoading() const { return isLoading_; }

void ChromiumEngineView::load(const QUrl &url) {
  if (webView_)
    webView_->load(url);
}

void ChromiumEngineView::reload() {
  if (webView_)
    webView_->reload();
}

void ChromiumEngineView::stop() {
  if (webView_)
    webView_->stop();
}

void ChromiumEngineView::back() {
  if (webView_)
    webView_->back();
}

void ChromiumEngineView::forward() {
  if (webView_)
    webView_->forward();
}

bool ChromiumEngineView::canGoBack() const {
  return webView_ && webView_->history() && webView_->history()->canGoBack();
}

bool ChromiumEngineView::canGoForward() const {
  return webView_ && webView_->history() && webView_->history()->canGoForward();
}

void ChromiumEngineView::runIsolatedJavaScript(const QString &script,
                                               std::function<void(const QVariant &)> callback) {
  if (!webView_ || !webView_->page()) {
    if (callback) callback(QVariant());
    return;
  }
  if (callback) webView_->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld, callback);
  else webView_->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld);
}

void ChromiumEngineView::runJavaScript(
    const QString &script, std::function<void(const QVariant &)> callback) {
  if (!webView_ || !webView_->page())
    return;
  if (callback) {
    webView_->page()->runJavaScript(script, callback);
  } else {
    webView_->page()->runJavaScript(script);
  }
}

qreal ChromiumEngineView::zoomFactor() const {
  return webView_ ? webView_->zoomFactor() : 1.0;
}

void ChromiumEngineView::setZoomFactor(qreal factor) {
  if (webView_)
    webView_->setZoomFactor(factor);
}

void ChromiumEngineView::findText(const QString &text, bool forward,
                                  bool caseSensitive) {
  if (!webView_ || !webView_->page())
    return;
  if (text.isEmpty()) {
    clearFind();
    return;
  }
  QWebEnginePage::FindFlags flags;
  if (!forward)
    flags |= QWebEnginePage::FindBackward;
  if (caseSensitive)
    flags |= QWebEnginePage::FindCaseSensitively;

  webView_->page()->findText(
      text, flags, [this](const QWebEngineFindTextResult &result) {
        emit findResultFound(result.activeMatch(), result.numberOfMatches());
      });
}

void ChromiumEngineView::clearFind() {
  if (webView_ && webView_->page()) {
    webView_->page()->findText(QString());
  }
  emit findResultFound(0, 0);
}

qint64 ChromiumEngineView::contentProcessId() const {
  return webView_ && webView_->page() ? webView_->page()->renderProcessPid() : 0;
}

void ChromiumEngineView::print() {
  if (webView_) {
    runJavaScript(QStringLiteral("window.print();"));
  }
}

void ChromiumEngineView::printToPdf(const QString &filePath) {
  if (webView_ && webView_->page()) {
    webView_->page()->printToPdf(filePath);
  }
}

} // namespace dalinira::engine
