#pragma once

#include <functional>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QTimer>
#include <QUrl>

#include "desktop_tabs/tab_resource_model.h"

namespace dalinira {

// The hover card's lines for a tab, the same wording for every engine.
struct TabResourceText {
  QString activity;      // "Durum: Aktif" / "Arka planda" / "Hafif uyku (etkinlik kısıtlı)" / "Uyuyor (bellekten çıkarıldı)"
  QString memory;        // "Bellek kullanımı: 320 MB"
  QString sharedMemory;  // "Paylaşılan işlem belleği: 1.4 GB (2 sekme)"; empty unless shared
  QString memoryBeforeSleep;  // "Uyumadan önce: 473 MB"; empty unless trustworthy
  QString cpu;           // "İşlemci kullanımı: %12"; empty unless measured
  QString diagnostics;   // pid, PSS and private memory, for developers only
};
TabResourceText describeTabResources(const TabResourceSnapshot &snapshot);
QString formatMemorySize(qint64 bytes);

}  // namespace dalinira

// Tab preview shown on hover: title, site, state and resource use. It renders
// the shared TabResourceSnapshot (desktop_tabs/tab_resource_model.h) and has
// no engine-specific code.
class TabHoverCard final : public QFrame {
  Q_OBJECT

 public:
  using SnapshotProvider = std::function<dalinira::TabResourceSnapshot()>;

  explicit TabHoverCard(QWidget *parent = nullptr);
  ~TabHoverCard() override = default;

  // `snapshot` is asked again while the card is visible, so state and memory
  // follow sleeping and waking. Internal (native Qt) pages have no snapshot.
  void showForTab(const QString &title, const QUrl &url, const QIcon &icon, const QRect &globalTabRect,
                  SnapshotProvider snapshot, bool isInternal = false);

  void hideCard();
  void refreshCardInfo();

  static QString extractDomain(const QUrl &url);
  static QRect boundedCardGeometry(const QRect &globalTabRect, const QSize &cardSize,
                                   const QRect &availableGeometry);

 protected:
  void paintEvent(QPaintEvent *event) override;
  void hideEvent(QHideEvent *event) override;

 private:
  QLabel *addRow(const QIcon &icon, const QString &objectName);

  QLabel *iconLabel_ = nullptr;
  QLabel *titleLabel_ = nullptr;
  QLabel *domainLabel_ = nullptr;
  QFrame *separator_ = nullptr;

  QLabel *audioTextLabel_ = nullptr;
  QLabel *lifecycleTextLabel_ = nullptr;
  QLabel *memoryTextLabel_ = nullptr;
  QLabel *sharedMemoryTextLabel_ = nullptr;
  QLabel *memoryBeforeSleepTextLabel_ = nullptr;
  QLabel *cpuTextLabel_ = nullptr;
  QLabel *diagnosticsLabel_ = nullptr;

  QTimer pollTimer_;
  SnapshotProvider snapshotProvider_;
  QRect globalTabRect_;
  QString fullTitle_;
  QString fullDomain_;
  bool isInternal_ = false;
  bool showDiagnostics_ = false;

  void updateContentGeometry();
  void updatePosition();
};
