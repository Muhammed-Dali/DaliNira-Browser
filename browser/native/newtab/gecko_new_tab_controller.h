#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
class BrowserWindow;
class BrowserProfileService;
namespace dalinira::engine {
class GeckoEngineView;
}
class GeckoNewTabController final : public QObject {
public:
  GeckoNewTabController(dalinira::engine::GeckoEngineView *view,
                        BrowserWindow *window, BrowserProfileService *profile);

private:
  void refresh(bool document = false);
  void action(const QJsonObject &event);
  bool active() const;
  QPointer<dalinira::engine::GeckoEngineView> view_;
  QPointer<BrowserWindow> window_;
  QPointer<BrowserProfileService> profile_;
  QJsonObject state_;
  QHash<QString, QUrl> items_;
  quint64 revision_ = 0;
  QByteArray providerConfiguration_;
};
