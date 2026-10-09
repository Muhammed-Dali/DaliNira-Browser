#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QWebEngineView>

class TabThrobber final : public QObject {
  Q_OBJECT

 public:
  using TabId = QUuid;

  struct ViewState {
    QPointer<const QObject> view;
    QPointer<QObject> ownerWindow;
    QIcon cachedFavicon;
    bool isLoading = false;
    bool visibleOnTabBar = false;
    qint64 startTimeMs = 0;
  };

  static TabThrobber &instance();

  explicit TabThrobber(QObject *parent = nullptr);
  ~TabThrobber() override = default;

  void startLoading(const QObject *view, QObject *ownerWindow, qint64 currentTimeMs = 0);
  void finishLoading(const QObject *view, bool success);
  void cacheFavicon(const QObject *view, const QIcon &icon);
  void updateOwner(const QObject *view, QObject *newOwner);
  void removeView(const QObject *view);

  bool isLoading(const QObject *view) const;
  bool isThrobberVisible(const QObject *view) const;
  QIcon cachedFavicon(const QObject *view) const;

  int loadingCount() const { return states_.size(); }
  bool isTimerActive() const { return animationTimer_.isActive(); }
  int frameStep() const { return frameStep_; }

  static QIcon renderThrobberIcon(int frameStep, const QPalette &palette, bool activeTab, qreal dpr = 1.0, int baseSize = 16);

 signals:
  void throbberTick();

 private slots:
  void onTimerTick();
  void onViewDestroyed(QObject *obj);

 private:
  ViewState *findState(const QObject *view);
  const ViewState *findState(const QObject *view) const;
  void updateTimerState();

  QHash<const QObject *, ViewState> states_;
  QTimer animationTimer_;
  int frameStep_ = 0;
};
