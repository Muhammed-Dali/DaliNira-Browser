#pragma once

#include "engine/engine_view.h"
#include <QPointer>
#include <QWebEngineView>

namespace dalinira::engine {

class ChromiumEngineView : public EngineView {
  Q_OBJECT
 public:
  explicit ChromiumEngineView(QWebEngineView *webView, QWidget *parent = nullptr);
  ~ChromiumEngineView() override;

  EngineType engineType() const override { return EngineType::Chromium; }
  QUrl url() const override;
  QString title() const override;
  bool isLoading() const override;

  void load(const QUrl &url) override;
  void reload() override;
  void stop() override;
  void back() override;
  void forward() override;
  bool canGoBack() const override;
  bool canGoForward() const override;

  void runJavaScript(const QString &script,
                     std::function<void(const QVariant &)> callback = {}) override;
  void runIsolatedJavaScript(const QString &script,
                             std::function<void(const QVariant &)> callback = {}) override;

  qreal zoomFactor() const override;
  void setZoomFactor(qreal factor) override;

  void findText(const QString &text, bool forward = true, bool caseSensitive = false) override;
  void clearFind() override;
  void print() override;
  qint64 contentProcessId() const override;
  void printToPdf(const QString &filePath) override;

  QWebEngineView *webView() const { return webView_.data(); }

 private:
  QPointer<QWebEngineView> webView_;
  bool isLoading_ = false;
};

} // namespace dalinira::engine
