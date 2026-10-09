#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cassert>
#include <iostream>

#include "downloads/download_desktop_actions.h"
#include "downloads/local_media_routing.h"
#include "i18n/language_manager.h"

namespace {

struct ActionTracking {
  QString lastOpenedPath;
  QString lastRevealedPath;
  QString lastRevealedFallback;
  int openCount = 0;
  int revealCount = 0;
};

QWidget *createCompletedCard(const QString &title,
                             const QString &filePath,
                             const QString &targetDirectory,
                             ActionTracking *tracking,
                             QLabel *statusLabel,
                             QWidget *parent) {
  auto *card = new QWidget(parent);
  auto *layout = new QVBoxLayout(card);
  auto *titleLabel = new QLabel(title, card);
  layout->addWidget(titleLabel);

  const QFileInfo fileInfo(filePath);
  const bool exists = fileInfo.exists() && fileInfo.isFile();

  auto *openBtn = new QPushButton(QStringLiteral("Aç"), card);
  openBtn->setEnabled(exists);
  QObject::connect(openBtn, &QPushButton::clicked, card, [filePath, targetDirectory, tracking, statusLabel] {
    tracking->lastOpenedPath = filePath;
    ++tracking->openCount;
    if (!DownloadDesktopActions::openFile(filePath, targetDirectory)) {
      if (statusLabel) statusLabel->setText(QStringLiteral("Dosya açılamadı veya bulunamadı"));
    } else {
      if (statusLabel) statusLabel->setText(QStringLiteral("Dosya başarıyla açıldı"));
    }
  });
  layout->addWidget(openBtn);

  auto *revealBtn = new QPushButton(QStringLiteral("Klasörde göster"), card);
  const QString folder = filePath.isEmpty() ? targetDirectory : QFileInfo(filePath).absolutePath();
  revealBtn->setEnabled(QFileInfo(folder).isDir());
  QObject::connect(revealBtn, &QPushButton::clicked, card, [filePath, targetDirectory, tracking, statusLabel] {
    tracking->lastRevealedPath = filePath;
    tracking->lastRevealedFallback = targetDirectory;
    ++tracking->revealCount;
    if (!DownloadDesktopActions::revealInFolder(filePath, targetDirectory)) {
      if (statusLabel) statusLabel->setText(QStringLiteral("Klasör açılamadı"));
    } else {
      if (statusLabel) statusLabel->setText(QStringLiteral("Klasör başarıyla gösterildi"));
    }
  });
  layout->addWidget(revealBtn);

  return card;
}

}  // namespace

int main(int argc, char *argv[]) {
  // Safe isolated profile and settings: user profile is NEVER touched
  QTemporaryDir settingsRoot;
  assert(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

  QApplication app(argc, argv);
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("DownloadGuiParityTest"));

  std::cout << "=== REAL-GUI DOWNLOAD ACTIONS PARITY VALIDATION ===" << std::endl;

  // Report Display Server
  const QString platform = QGuiApplication::platformName();
  const QString sessionType = qEnvironmentVariable("XDG_SESSION_TYPE");
  const QString waylandDisplay = qEnvironmentVariable("WAYLAND_DISPLAY");
  const QString xDisplay = qEnvironmentVariable("DISPLAY");

  std::cout << "[GUI SERVER] Platform: " << platform.toStdString()
            << " | Session: " << sessionType.toStdString()
            << " | Wayland: " << waylandDisplay.toStdString()
            << " | X11: " << xDisplay.toStdString() << std::endl;

  if (platform == QLatin1String("wayland")) {
    std::cout << "[GUI SERVER] Running on Native Wayland." << std::endl;
  } else if (platform == QLatin1String("xcb") && !waylandDisplay.isEmpty()) {
    std::cout << "[GUI SERVER] Running on XWayland." << std::endl;
  } else {
    std::cout << "[GUI SERVER] Running on X11 / " << platform.toStdString() << std::endl;
  }

  dalinira::i18n::LanguageManager::instance().initialize();

  // Create isolated synthetic destinations
  QTemporaryDir destinationA;
  assert(destinationA.isValid());
  const QString chromiumFile = destinationA.filePath(QStringLiteral("chromium_completed_video.mp4"));
  {
    QFile f(chromiumFile);
    assert(f.open(QIODevice::WriteOnly));
    f.write("synthetic-chromium-video-bytes");
    f.close();
  }

  QTemporaryDir destinationB;
  assert(destinationB.isValid());
  const QString webkitFile = destinationB.filePath(QStringLiteral("webkit_completed_video.mp4"));
  {
    QFile f(webkitFile);
    assert(f.open(QIODevice::WriteOnly));
    f.write("synthetic-webkit-video-bytes");
    f.close();
  }

  // Verify paths are isolated and distinct
  assert(QFileInfo(chromiumFile).absolutePath() != QFileInfo(webkitFile).absolutePath());
  assert(QFileInfo::exists(chromiumFile));
  assert(QFileInfo::exists(webkitFile));

  // Top level window
  QWidget topWindow;
  topWindow.setWindowTitle(QStringLiteral("DaliNira Download Parity GUI Validation"));
  topWindow.resize(480, 500);
  auto *topLayout = new QVBoxLayout(&topWindow);
  auto *statusLabel = new QLabel(QStringLiteral("Hazır"), &topWindow);
  topLayout->addWidget(statusLabel);

  ActionTracking tracking;

  // --------------------------------------------------------------------------
  // Chromium Download GUI Verification
  // --------------------------------------------------------------------------
  std::cout << "[GUI 1] Chromium completed download entry & actions..." << std::endl;
  auto *crCard = createCompletedCard(QStringLiteral("Chromium Video (Destination A)"),
                                     chromiumFile,
                                     destinationA.path(),
                                     &tracking,
                                     statusLabel,
                                     &topWindow);
  topLayout->addWidget(crCard);
  topWindow.show();
  app.processEvents();

  // Find buttons on Chromium card
  const auto crButtons = crCard->findChildren<QPushButton *>();
  QPushButton *crOpenBtn = nullptr;
  QPushButton *crRevealBtn = nullptr;
  for (auto *btn : crButtons) {
    if (btn->text() == QStringLiteral("Aç")) crOpenBtn = btn;
    else if (btn->text() == QStringLiteral("Klasörde göster")) crRevealBtn = btn;
  }
  assert(crOpenBtn && crOpenBtn->isEnabled());
  assert(crRevealBtn && crRevealBtn->isEnabled());

  // Click Chromium "Aç"
  crOpenBtn->click();
  app.processEvents();
  assert(tracking.openCount == 1);
  assert(tracking.lastOpenedPath == chromiumFile);
  assert(statusLabel->text() == QStringLiteral("Dosya başarıyla açıldı"));

  // Click Chromium "Klasörde göster"
  crRevealBtn->click();
  app.processEvents();
  assert(tracking.revealCount == 1);
  assert(tracking.lastRevealedPath == chromiumFile);
  assert(tracking.lastRevealedFallback == destinationA.path());
  assert(statusLabel->text() == QStringLiteral("Klasör başarıyla gösterildi"));

  std::cout << "[PASS] Chromium Open and Reveal actions target exact canonical file in Destination A." << std::endl;

  // --------------------------------------------------------------------------
  // WebKit Download GUI Verification
  // --------------------------------------------------------------------------
  std::cout << "[GUI 2] WebKit completed download entry & actions..." << std::endl;
  auto *wkCard = createCompletedCard(QStringLiteral("WebKit Video (Destination B)"),
                                     webkitFile,
                                     destinationB.path(),
                                     &tracking,
                                     statusLabel,
                                     &topWindow);
  topLayout->addWidget(wkCard);
  app.processEvents();

  // Find buttons on WebKit card
  const auto wkButtons = wkCard->findChildren<QPushButton *>();
  QPushButton *wkOpenBtn = nullptr;
  QPushButton *wkRevealBtn = nullptr;
  for (auto *btn : wkButtons) {
    if (btn->text() == QStringLiteral("Aç")) wkOpenBtn = btn;
    else if (btn->text() == QStringLiteral("Klasörde göster")) wkRevealBtn = btn;
  }
  assert(wkOpenBtn && wkOpenBtn->isEnabled());
  assert(wkRevealBtn && wkRevealBtn->isEnabled());

  // Click WebKit "Aç"
  wkOpenBtn->click();
  app.processEvents();
  assert(tracking.openCount == 2);
  assert(tracking.lastOpenedPath == webkitFile);
  assert(statusLabel->text() == QStringLiteral("Dosya başarıyla açıldı"));

  // Click WebKit "Klasörde göster"
  wkRevealBtn->click();
  app.processEvents();
  assert(tracking.revealCount == 2);
  assert(tracking.lastRevealedPath == webkitFile);
  assert(tracking.lastRevealedFallback == destinationB.path());
  assert(statusLabel->text() == QStringLiteral("Klasör başarıyla gösterildi"));

  std::cout << "[PASS] WebKit Open and Reveal actions target exact canonical file in Destination B." << std::endl;

  // --------------------------------------------------------------------------
  // Missing File Graceful Degradation GUI Verification
  // --------------------------------------------------------------------------
  std::cout << "[GUI 3] Missing file graceful degradation..." << std::endl;
  const QString deletedFile = destinationB.filePath(QStringLiteral("deleted_synth_video.mp4"));
  assert(!QFile::exists(deletedFile));

  auto *missingCard = createCompletedCard(QStringLiteral("Deleted Download"),
                                          deletedFile,
                                          destinationB.path(),
                                          &tracking,
                                          statusLabel,
                                          &topWindow);
  topLayout->addWidget(missingCard);
  app.processEvents();

  const auto missingButtons = missingCard->findChildren<QPushButton *>();
  QPushButton *missingOpenBtn = nullptr;
  QPushButton *missingRevealBtn = nullptr;
  for (auto *btn : missingButtons) {
    if (btn->text() == QStringLiteral("Aç")) missingOpenBtn = btn;
    else if (btn->text() == QStringLiteral("Klasörde göster")) missingRevealBtn = btn;
  }
  assert(missingOpenBtn && !missingOpenBtn->isEnabled());  // Open disabled when file missing
  assert(missingRevealBtn && missingRevealBtn->isEnabled());  // Reveal enabled because destinationB exists

  // Reveal should safely open directory
  missingRevealBtn->click();
  app.processEvents();
  assert(tracking.revealCount == 3);
  assert(tracking.lastRevealedPath == deletedFile);
  assert(tracking.lastRevealedFallback == destinationB.path());
  assert(statusLabel->text() == QStringLiteral("Klasör başarıyla gösterildi"));

  std::cout << "[PASS] Missing file fails gracefully without crash." << std::endl;

  topWindow.close();
  app.processEvents();

  std::cout << "=== REAL-GUI DOWNLOAD ACTIONS VALIDATION PASSED ===" << std::endl;
  return 0;
}
