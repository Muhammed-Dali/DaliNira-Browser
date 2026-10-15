#pragma once

#include <QList>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <functional>

#include "engine/engine_view.h"

namespace dalinira::engine {

class GeckoBackend;
class GeckoRequest;
class GeckoSessionView;

// A browser tab on the experimental Gecko engine: one tab of the Gecko host
// (GeckoSessionView), shown through the host's shared surface while it is
// the active tab of its window.
//
// The host tab is asynchronous: until it exists, commands wait in order (the
// first http(s) URL goes with the tab's creation). Gecko pages are http(s)
// only in ordinary URL commands; the named DaliNira New Tab document uses
// a separate feature-negotiated, identity-bound internal-page API. A crashed page is recovered by reload(); a page
// with unsaved changes closes only after the user confirmed (requestClose).
class GeckoEngineView : public EngineView {
  Q_OBJECT

 public:
  explicit GeckoEngineView(GeckoBackend *backend, QWidget *parent = nullptr);
  ~GeckoEngineView() override;

  EngineType engineType() const override { return EngineType::Gecko; }
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

  void setEngineActive(bool active) override;
  bool isEngineActive() const override { return engineActive_; }
  void requestClose(std::function<void(bool allowed)> done) override;
  bool isAudible() const override;
  qint64 contentProcessId() const override;

  void prepareNewTab(const QByteArray &markup, const QJsonObject &state);
  void updateNewTabState(const QJsonObject &state);
  void commitNewTabAction(const QJsonObject &event, const QJsonObject &effect, std::function<void()> committed);
  bool isHostSelectionConfirmed() const { return engineActive_ && activationConfirmed_ && activationRequest_.isNull(); }
  QString hostTabId() const;
  bool isCrashed() const;
  // Why the page is not shown (no host tab yet, Gecko stopped, another window).
  QString status() const { return status_; }

 signals:
  void newTabRequested();
  void internalAction(const QJsonObject &event);
  // A page of this tab opened a tab of its own (target=_blank, window.open)
  // in the host: the browser shows it as a new tab (GeckoBackend::adoptHostTab).
  void contentTabOpened(const QString &hostTabId);

 protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void hideEvent(QHideEvent *event) override;

 private:
  friend class GeckoBackend;
  using Command = std::function<GeckoRequest *(GeckoSessionView *)>;

  void createHostTab();
  void bind(GeckoSessionView *view);
  void run(Command command);
  void flush();
  void present();
  void setStatus(const QString &status);
  void hostTabFailed(const QString &reason);

  QPointer<GeckoBackend> backend_;
  QPointer<GeckoSessionView> view_;
  QUrl requestedUrl_;     // what the browser asked for (may be dalinira://)
  QUrl initialUrl_;       // an http(s) URL for the host tab's creation
  QList<Command> queue_;  // waits for the host tab
  QString status_;
  bool engineActive_ = false;
  bool creating_ = false;
  bool closing_ = false;
  QByteArray pendingNewTab_;
  QJsonObject newTabState_;
  bool newTabLoading_ = false;
  quint64 presentationEpoch_ = 0;
  bool activationConfirmed_ = false;
  QPointer<GeckoRequest> activationRequest_;
  void publishNewTab();
};

}  // namespace dalinira::engine
