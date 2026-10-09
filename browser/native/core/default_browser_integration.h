#pragma once

#include <QObject>

#include <functional>

namespace dalinira::core {

inline constexpr auto kDefaultBrowserPromptDisabledKey =
    "browser/defaultBrowserPromptDisabled";
inline constexpr auto kDefaultBrowserLastPromptUtcKey =
    "browser/defaultBrowserLastPromptUtc";

enum class DefaultBrowserResult {
  Success,
  AlreadyDefault,
  Unsupported,
  Failed
};

class IDefaultBrowserIntegration {
 public:
  virtual ~IDefaultBrowserIntegration() = default;
  virtual bool isSupported() const = 0;
  virtual void requestDefaultBrowser(
      std::function<void(DefaultBrowserResult)> completion) = 0;
};

// Uses the desktop environment's xdg-settings integration and verifies the
// result before reporting success. Other platforms are explicitly unsupported.
class SystemDefaultBrowserIntegration final : public QObject,
                                              public IDefaultBrowserIntegration {
 public:
  explicit SystemDefaultBrowserIntegration(QObject *parent = nullptr);

  bool isSupported() const override;
  void requestDefaultBrowser(
      std::function<void(DefaultBrowserResult)> completion) override;

  static QString desktopFileName();

 private:
  QString executablePath() const;
  void verify(std::function<void(DefaultBrowserResult)> completion,
              bool settingWasRequested);
};

}  // namespace dalinira::core
