#pragma once

#include "download_ui_model.h"
#include "local_media_routing.h"

#include <QFrame>
#include <QPointer>
#include <QToolButton>
#include <QUrl>

class QLabel;
class QPropertyAnimation;
class QScrollArea;
class QTimer;
class QVBoxLayout;

class DownloadToolbarButton final : public QToolButton {
  Q_OBJECT
  Q_PROPERTY(qreal pulse READ pulse WRITE setPulse)
 public:
  explicit DownloadToolbarButton(QWidget *parent = nullptr);
  void setModelState(int activeCount, double progress, bool paused, bool error);
  int activeCount() const { return activeCount_; }
  double displayedProgress() const { return progress_; }
  qreal pulse() const { return pulse_; }
  void setPulse(qreal value);
  void acknowledge(bool animationsEnabled);

 protected:
  void paintEvent(QPaintEvent *event) override;

 private:
  int activeCount_ = 0;
  double progress_ = 0.0;
  bool paused_ = false;
  bool error_ = false;
  qreal pulse_ = 0.0;
  QPointer<QPropertyAnimation> pulseAnimation_;
};

class DownloadPopup final : public QFrame {
  Q_OBJECT
 public:
  explicit DownloadPopup(DownloadUiModel *model, QWidget *parent = nullptr);
  void showAnchored(QWidget *anchor, bool transient);
  void reposition(QWidget *anchor);
  void setSuggestedMedia(const QUrl &url, const QString &title);

 signals:
  void openDownloadsRequested();
  void openMediaDownloadRequested(const QUrl &url);
  void openLocalMediaRequested(const LocalMediaOpenRequest &request);

 protected:
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;

 private:
  void refresh();
  void scheduleAutoClose();
  DownloadUiModel *model_ = nullptr;
  QPointer<QWidget> anchor_;
  QVBoxLayout *itemsLayout_ = nullptr;
  QLabel *emptyLabel_ = nullptr;
  QFrame *suggestedMediaCard_ = nullptr;
  QUrl suggestedMediaUrl_;
  QString suggestedMediaTitle_;
  QTimer *autoCloseTimer_ = nullptr;
  bool transient_ = false;
  QString structureSignature_;
};
