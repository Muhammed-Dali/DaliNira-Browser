#include "default_browser_integration.h"

#include <QProcess>
#include <QStandardPaths>

#include <memory>

namespace dalinira::core {

namespace {

using Completion = std::function<void(DefaultBrowserResult)>;

void runProcess(QObject *owner, const QString &program, const QStringList &arguments,
                std::function<void(bool, QByteArray)> completion) {
  auto *process = new QProcess(owner);
  auto finished = std::make_shared<bool>(false);
  auto finishOnce = [process, finished, completion = std::move(completion)](
                        bool success, const QByteArray &output) mutable {
    if (*finished) return;
    *finished = true;
    completion(success, output);
    process->deleteLater();
  };
  QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                   owner, [process, finishOnce](int exitCode, QProcess::ExitStatus status) mutable {
    finishOnce(status == QProcess::NormalExit && exitCode == 0,
               process->readAllStandardOutput());
  });
  QObject::connect(process, &QProcess::errorOccurred, owner,
                   [finishOnce](QProcess::ProcessError error) mutable {
    if (error == QProcess::FailedToStart) finishOnce(false, {});
  });
  process->start(program, arguments);
}

}  // namespace

SystemDefaultBrowserIntegration::SystemDefaultBrowserIntegration(QObject *parent)
    : QObject(parent) {}

QString SystemDefaultBrowserIntegration::desktopFileName() {
  return QStringLiteral("dalinira.desktop");
}

QString SystemDefaultBrowserIntegration::executablePath() const {
#if defined(Q_OS_LINUX)
  return QStandardPaths::findExecutable(QStringLiteral("xdg-settings"));
#else
  return {};
#endif
}

bool SystemDefaultBrowserIntegration::isSupported() const {
  return !executablePath().isEmpty();
}

void SystemDefaultBrowserIntegration::requestDefaultBrowser(Completion completion) {
  const QString program = executablePath();
  if (program.isEmpty()) {
    completion(DefaultBrowserResult::Unsupported);
    return;
  }

  verify([this, program, completion = std::move(completion)](
             DefaultBrowserResult result) mutable {
    if (result == DefaultBrowserResult::AlreadyDefault) {
      completion(result);
      return;
    }
    runProcess(this, program,
               {QStringLiteral("set"), QStringLiteral("default-web-browser"),
                desktopFileName()},
               [this, completion = std::move(completion)](bool success, const QByteArray &) mutable {
      if (!success) {
        completion(DefaultBrowserResult::Failed);
        return;
      }
      verify(std::move(completion), true);
    });
  }, false);
}

void SystemDefaultBrowserIntegration::verify(Completion completion,
                                              bool settingWasRequested) {
  const QString program = executablePath();
  if (program.isEmpty()) {
    completion(DefaultBrowserResult::Unsupported);
    return;
  }
  runProcess(this, program,
             {QStringLiteral("get"), QStringLiteral("default-web-browser")},
             [completion = std::move(completion), settingWasRequested](
                 bool success, const QByteArray &output) mutable {
    if (!success) {
      completion(DefaultBrowserResult::Failed);
      return;
    }
    const bool matches = QString::fromUtf8(output).trimmed()
        == SystemDefaultBrowserIntegration::desktopFileName();
    if (matches) {
      completion(settingWasRequested ? DefaultBrowserResult::Success
                                     : DefaultBrowserResult::AlreadyDefault);
    } else {
      completion(settingWasRequested ? DefaultBrowserResult::Failed
                                     : DefaultBrowserResult::Failed);
    }
  });
}

}  // namespace dalinira::core
