#pragma once

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <memory>

#include "engine/engine_backend.h"
#include "engine/gecko/gecko_host_process.h"
#include "engine/gecko/gecko_runtime.h"

namespace dalinira::engine {

class GeckoEngineView;
class GeckoRequest;
class GeckoPresentationSurface;
class GeckoSession;
class GeckoSessionView;
class GeckoShortcutRouter;

// The experimental Gecko engine: one Gecko host process (GeckoSession) for
// the whole browser, one host tab per GeckoEngineView, one shared native
// surface (GeckoPresentationSurface). Gecko runs only in the host process.
//
// Storage (<AppData>/gecko): the Gecko profile, kept across launches
// (profile/, private), the launches' run directories (runtime/, private,
// removed when their host stops) and the host's log (0600). The IPC socket and its
// one-shot token live in a private directory of XDG_RUNTIME_DIR.
// No private (ephemeral) views: createEphemeralView() refuses.
class GeckoBackend : public EngineBackend {
  Q_OBJECT

 public:
  struct Config {
    GeckoRuntimePaths runtime = GeckoRuntime::developmentPaths();
    QString dataDirectory;  // empty: EngineStorageManager::engineDir(EngineType::Gecko)
    QString ipcRoot;        // empty: XDG_RUNTIME_DIR
    GeckoHostProcess::Display display = GeckoHostProcess::Display::X11;
    bool testInput = false;  // GeckoHostProcess::Config::testInput (tests only)
  };
  // Asks the user whether to discard a page's unsaved changes.
  using DiscardConfirmation = std::function<bool(QWidget *parent, const QString &title)>;

  explicit GeckoBackend(Config config, QObject *parent = nullptr);
  ~GeckoBackend() override;

  EngineType engineType() const override { return EngineType::Gecko; }
  QString name() const override { return QStringLiteral("Gecko"); }
  QString layoutEngine() const override { return QStringLiteral("Gecko"); }
  QString jsEngine() const override { return QStringLiteral("SpiderMonkey"); }
  QString versionString() const override;
  bool supportsCapability(EngineCapability cap) const override;

  // Prepares the Gecko storage and starts the host (ready later).
  bool initialize(const EngineStorageManager &storage) override;
  EngineView *createView(QWidget *parent = nullptr) override;
  EngineView *createEphemeralView(QWidget *parent = nullptr) override;
  EngineProfile *profile() const override { return profile_.get(); }

  GeckoSession *session() const { return session_; }
  GeckoPresentationSurface *surface() const { return surface_; }
  GeckoShortcutRouter *shortcutRouter() const { return shortcutRouter_; }
  QString lastError() const { return lastError_; }
  QString dataDirectory() const { return dataDirectory_; }

  // The next createView() shows this host tab (one a page opened).
  void adoptHostTab(const QString &tabId) { adoptNext_ = tabId; }
  // Guarded session shutdown, separate from final resource cleanup.
  void requestShutdown(QWidget *parent, std::function<void(bool)> done);
  // At exit: guarded shutdown, then bounded cleanup only after acceptance.
  bool shutdownHost(int timeoutMs);

  void setDiscardConfirmation(DiscardConfirmation confirmation) { confirmation_ = std::move(confirmation); }
  bool confirmDiscard(QWidget *parent, const QString &title) const;

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

 private:
  bool shutdownPending_ = false;
  friend class GeckoEngineView;
  friend class GeckoShortcutRouter;
  void registerView(GeckoEngineView *view);
  void unregisterView(GeckoEngineView *view);
  // A host tab no view shows yet for `view` (its adopted tab, else the host's
  // own first tab); null: create one.
  GeckoSessionView *takeHostTab(GeckoEngineView *view);
  void expectCreatedTab(GeckoRequest *request, GeckoEngineView *view);
  void onViewCreated(GeckoSessionView *view, const QString &source);
  GeckoEngineView *viewShowing(const QString &tabId) const;

  Config config_;
  GeckoSession *session_ = nullptr;
  GeckoPresentationSurface *surface_ = nullptr;
  GeckoShortcutRouter *shortcutRouter_ = nullptr;
  std::unique_ptr<EngineProfile> profile_;
  QList<QPointer<GeckoEngineView>> views_;
  QHash<const GeckoEngineView *, QString> adopted_;  // view → host tab it adopts
  QStringList spareTabs_;    // host tabs of the host's own (first tab, replacements)
  QStringList contentTabs_;  // tabs pages opened, until a view adopts them
  QHash<QString, QPointer<GeckoEngineView>> expectedCreations_;  // request id → view
  QString adoptNext_;
  QString dataDirectory_;
  QString milestone_;
  QString lastError_;
  DiscardConfirmation confirmation_;
};

}  // namespace dalinira::engine
