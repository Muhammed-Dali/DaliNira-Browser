#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include "engine/gecko/gecko_runtime.h"

namespace dalinira::engine {

// Launches the DaliNira Gecko Host (the validated runtime's `firefox` running
// the host application) and owns its whole process tree. Gecko never runs
// inside DaliNira.
//
// Each launch gets a private run directory (0700): the profile, a HOME and
// XDG directories, TMPDIR and the host's output log. The fresh profile points
// Gecko's policy engine at the host's update-disable policy. The environment
// is built from an allowlist (no LD_*, GTK_MODULES, QT_* or inherited MOZ_*
// variables reach Gecko). The child dies with DaliNira (PR_SET_PDEATHSIG),
// never writes core files, and inherits no file descriptors. Only processes
// verified to descend from the launched host (pid and start time) are ever
// signalled.
class GeckoHostProcess : public QObject {
  Q_OBJECT

 public:
  enum class Display {
    // The host's window on the X11 server DaliNira uses (XWayland on
    // Wayland sessions); it is embedded through GeckoX11Embedder.
    X11,
    // No display connection and no GPU access at all (MOZ_HEADLESS and
    // MOZ_AVOID_OPENGL_ALTOGETHER): for automated tests.
    Headless,
  };

  struct Config {
    GeckoValidatedRuntime runtime;
    QString runDirectory;  // exists, private (0700); the launch's files go here
    QString socketPath;    // GeckoIpcServer
    QString tokenFile;     // GeckoIpcServer
    Display display = Display::X11;
  };

  struct ProcessId {
    qint64 pid = 0;
    quint64 startTime = 0;
  };

  explicit GeckoHostProcess(QObject *parent = nullptr);
  ~GeckoHostProcess() override;

  bool start(const Config &config, QString *error);
  qint64 pid() const { return pid_; }
  bool isRunning() const { return process_.state() != QProcess::NotRunning; }
  QString profileDirectory() const { return profileDirectory_; }

  // Processes descending from the host right now.
  QList<ProcessId> descendants() const;

  // Waits up to `graceMs` for the host to exit (after an IPC shutdown), then
  // SIGTERM, then SIGKILL; finally SIGKILLs any verified descendant left.
  QJsonObject stop(int graceMs);

  // The host's environment: an allowlist of `system` plus the launch's own
  // settings. Empty with `error` set when the display is unusable.
  static QProcessEnvironment environmentFor(const QProcessEnvironment &system, const Config &config,
                                            const QString &homeDirectory, QString *error);
  static QStringList argumentsFor(const Config &config, const QString &profileDirectory);

 signals:
  void exited(int exitCode, QProcess::ExitStatus status);

 private:
  QProcess process_;
  QString profileDirectory_;
  qint64 pid_ = 0;
  bool stopping_ = false;
};

}  // namespace dalinira::engine
