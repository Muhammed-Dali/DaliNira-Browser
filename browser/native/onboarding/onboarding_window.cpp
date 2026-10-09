#include "onboarding_window.h"
#include "onboarding_view.h"
#include "onboarding_manager.h"
#include "core/browser_icons.h"
#include "layout/browser_layout.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace dalinira::onboarding {

OnboardingWindow::OnboardingWindow(IBrowserDataReceiver *dataReceiver,
                                   ISearchPrivacyPreferences *searchPrivacyPreferences,
                                   dalinira::core::IDefaultBrowserIntegration *defaultBrowser,
                                   QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint),
      dataReceiver_(dataReceiver),
      searchPrivacyPreferences_(searchPrivacyPreferences),
      defaultBrowser_(defaultBrowser) {
  setupUi();
}

void OnboardingWindow::setupUi() {
  setObjectName(QStringLiteral("onboarding-window"));
  setWindowTitle(dalinira::i18n::I18n::text(QStringLiteral("onboarding.welcome_title")));

  const QIcon icon = BrowserIcons::appIcon();
  if (!icon.isNull()) {
    setWindowIcon(icon);
  }

  setMinimumSize(960, 680);

  // Use most of the available desktop without entering maximized/full-screen
  // mode. This keeps the setup focused while leaving visible space around it.
  if (const QScreen *screen = QGuiApplication::primaryScreen()) {
    const QRect screenGeometry = screen->availableGeometry();
    const int targetWidth = std::clamp(qRound(screenGeometry.width() * 0.90),
                                       minimumWidth(), 1720);
    const int targetHeight = std::clamp(qRound(screenGeometry.height() * 0.90),
                                        minimumHeight(), 960);
    resize(targetWidth, targetHeight);
    const int x = screenGeometry.x() + (screenGeometry.width() - width()) / 2;
    const int y = screenGeometry.y() + (screenGeometry.height() - height()) / 2;
    move(x, y);
  } else {
    resize(1280, 800);
  }

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  onboardingView_ = new OnboardingView(dataReceiver_, searchPrivacyPreferences_, this, defaultBrowser_);
  rootLayout->addWidget(onboardingView_);

  closeButton_ = new QToolButton(this);
  closeButton_->setObjectName(QStringLiteral("onboarding-close-button"));
  closeButton_->setText(QString::fromUtf8("\u00d7"));
  closeButton_->setToolTip(dalinira::i18n::I18n::text(QStringLiteral("common.close")));
  closeButton_->setAccessibleName(dalinira::i18n::I18n::text(QStringLiteral("common.close")));
  closeButton_->setCursor(Qt::PointingHandCursor);
  closeButton_->setFixedSize(44, 44);
  closeButton_->setStyleSheet(QStringLiteral(
      "#onboarding-close-button {"
      "  background: transparent;"
      "  color: rgba(255, 255, 255, 0);"
      "  border: none;"
      "  border-radius: 10px;"
      "  font-size: 26px;"
      "  font-weight: 400;"
      "}"
      "#onboarding-close-button:hover, #onboarding-close-button:focus {"
      "  background: rgba(239, 68, 68, 0.90);"
      "  color: #ffffff;"
      "}"));
  connect(closeButton_, &QToolButton::clicked, this, &OnboardingWindow::close);
  closeButton_->move(width() - closeButton_->width() - 10, 10);
  closeButton_->raise();

  connect(onboardingView_, &OnboardingView::finished, this, [this] {
    completedOrSkipped_ = true;
    emit onboardingFinished();
    close();
  });

  connect(&OnboardingManager::instance(), &OnboardingManager::themeSelectionChanged, this,
          [](const QString &themeString) {
    const auto theme = dalinira::layout::appearanceThemeFromString(themeString);
    dalinira::layout::applyApplicationTheme(qobject_cast<QApplication *>(qApp), theme);
  });

  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] {
    setWindowTitle(dalinira::i18n::I18n::text(QStringLiteral("onboarding.welcome_title")));
    const QString closeText = dalinira::i18n::I18n::text(QStringLiteral("common.close"));
    closeButton_->setToolTip(closeText);
    closeButton_->setAccessibleName(closeText);
  });
}

void OnboardingWindow::closeEvent(QCloseEvent *event) {
  if (!completedOrSkipped_) {
    emit onboardingDismissed();
  }
  event->accept();
}

void OnboardingWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (closeButton_) {
    closeButton_->move(width() - closeButton_->width() - 10, 10);
    closeButton_->raise();
  }
}

}  // namespace dalinira::onboarding
