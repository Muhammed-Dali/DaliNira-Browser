#include "browser_window.h"
#include "browser_window_internal.h"
#include "core/browser_icons.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "desktop_tabs/find_bar_widget.h"
#include "engine/engine_view.h"
#include "i18n/i18n.h"

#include <algorithm>

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPointer>
#include <QPrinter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTextBrowser>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>
#include <QWebEngineFullScreenRequest>

using dalinira::i18n::I18n;

void BrowserWindow::showFindBar() {
  if (!findBar_) {
    findBar_ = new dalinira::desktop_tabs::FindBarWidget(this);
    connect(findBar_, &dalinira::desktop_tabs::FindBarWidget::findRequested,
            this, &BrowserWindow::handleFindRequest);
    connect(findBar_, &dalinira::desktop_tabs::FindBarWidget::clearFindRequested,
            this, &BrowserWindow::handleClearFind);
    connect(findBar_, &dalinira::desktop_tabs::FindBarWidget::closeRequested,
            this, &BrowserWindow::hideFindBar);
  }
  const bool reopening = findBar_->isHidden();
  findBar_->refreshAppearance();
  // A WebKit page makes the page stack a native X window; an alien popup would
  // be painted under it. A native popup stacks above it once raised.
  if (pageStack_ && pageStack_->internalWinId() && !findBar_->internalWinId()) findBar_->winId();
  updateFindBarPosition();
  findBar_->show();
  findBar_->raise();

  const auto usableSelection = [](const QString &text) {
    const QString trimmed = text.trimmed();
    return trimmed.contains(QLatin1Char('\n')) ? QString() : trimmed;
  };
  const QString selected = currentView() ? usableSelection(currentView()->selectedText()) : QString();
  if (!selected.isEmpty()) {
    findBar_->setFindText(selected);
    handleFindRequest(selected, true, findBar_->isCaseSensitive());
  } else if (reopening && !findBar_->findText().isEmpty()) {
    // Closing cleared the highlights; the kept query searches again.
    handleFindRequest(findBar_->findText(), true, findBar_->isCaseSensitive());
  }
  findBar_->focusAndSelectAll();

  // Engines without a synchronous selection (WebKit) prefill once the page
  // answers, unless the user has started typing meanwhile.
  auto *engineView = currentEngineView();
  if (!currentView() && engineView) {
    const QPointer<dalinira::desktop_tabs::FindBarWidget> bar = findBar_;
    const QString queryAtOpen = findBar_->findText();
    engineView->runIsolatedJavaScript(
        QStringLiteral("String(window.getSelection ? window.getSelection() : '')"),
        [bar, queryAtOpen, usableSelection](const QVariant &value) {
          const QString text = usableSelection(value.toString());
          if (!bar || !bar->isVisible() || text.isEmpty() || bar->findText() != queryAtOpen ||
              text == queryAtOpen)
            return;
          bar->setFindText(text);
          bar->focusAndSelectAll();
        });
  }
}

void BrowserWindow::hideFindBar() {
  if (findBar_) {
    findBar_->hide();
  }
  handleClearFind();
  if (auto *engineView = currentEngineView()) {
    engineView->setFocus(Qt::ShortcutFocusReason);
  } else if (auto *view = currentView()) {
    view->setFocus();
  }
}

void BrowserWindow::updateFindBarPosition() {
  if (!findBar_) return;
  constexpr int horizontalMargin = 12;
  constexpr int verticalGap = 7;
  const int availableWidth = std::max(220, width() - horizontalMargin * 2);
  const int popupWidth = std::min(findBar_->sizeHint().width(), availableWidth);
  const int popupHeight = findBar_->sizeHint().height();
  const int navBottom = navBar_
      ? navBar_->mapTo(this, QPoint(0, navBar_->height())).y()
      : 80;
  const int top = std::min(std::max(0, height() - popupHeight), navBottom + verticalGap);
  // Hang under the toolbar's find button like the other toolbar popups, but
  // never past the window edge (narrow windows, button hidden in fullscreen).
  int rightEdge = width() - horizontalMargin;
  if (findButton_ && findButton_->isVisible()) {
    rightEdge = std::min(rightEdge, findButton_->mapTo(this, QPoint(findButton_->width(), 0)).x());
  }
  const int left = std::clamp(rightEdge - popupWidth, horizontalMargin,
                              std::max(horizontalMargin, width() - horizontalMargin - popupWidth));
  findBar_->setGeometry(left, top, popupWidth, popupHeight);
}

void BrowserWindow::handleFindRequest(const QString &text, bool forward, bool caseSensitive) {
  if (auto *ev = currentEngineView()) {
    if (text.isEmpty()) {
      handleClearFind();
      return;
    }
    ev->findText(text, forward, caseSensitive);
    return;
  }

  auto *view = currentView();
  if (!view || !view->page()) return;

  if (text.isEmpty()) {
    handleClearFind();
    return;
  }

  QWebEnginePage::FindFlags flags;
  if (!forward) flags |= QWebEnginePage::FindBackward;
  if (caseSensitive) flags |= QWebEnginePage::FindCaseSensitively;

  view->page()->findText(text, flags, [this](const QWebEngineFindTextResult &result) {
    if (findBar_) {
      findBar_->setMatchCount(result.activeMatch(), result.numberOfMatches());
    }
  });
}

void BrowserWindow::handleClearFind() {
  if (auto *ev = currentEngineView()) {
    ev->clearFind();
  }
  if (auto *view = currentView()) {
    if (view->page()) {
      view->page()->findText(QString());
    }
  }
  if (findBar_) {
    findBar_->clearMatchCount();
  }
}

void BrowserWindow::printCurrentPage() {
  if (auto *ev = currentEngineView()) {
    ev->print();
    return;
  }
  auto *view = currentView();
  if (!view || !view->page()) return;
  auto printer = std::make_shared<QPrinter>(QPrinter::HighResolution);
  QPrintDialog dialog(printer.get(), this);
  dialog.setWindowTitle(I18n::text(QStringLiteral("print.dialog_title")));
  if (dialog.exec() == QDialog::Accepted) {
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(view, &QWebEngineView::printFinished, view, [printer, conn](bool success) {
      Q_UNUSED(success);
      QObject::disconnect(*conn);
    });
    view->print(printer.get());
  }
}

void BrowserWindow::printCurrentPageToPdf() {
  if (auto *ev = currentEngineView()) {
    QString title = ev->title().trimmed();
    if (title.isEmpty()) title = I18n::text(QStringLiteral("page.default_file_name"));
    title.replace(QRegularExpression(QStringLiteral("[/\\\\?%*:|\"<>]")), QStringLiteral("_"));
    const QString defaultPath = QDir::homePath() + QLatin1Char('/') + title + QStringLiteral(".pdf");
    const QString filePath = QFileDialog::getSaveFileName(this,
        I18n::text(QStringLiteral("print.pdf_save_title")),
        defaultPath,
        I18n::text(QStringLiteral("print.pdf_filter")));
    if (!filePath.isEmpty()) {
      ev->printToPdf(filePath);
    }
    return;
  }
  auto *view = currentView();
  if (!view || !view->page()) return;
  QString title = view->title().trimmed();
  if (title.isEmpty()) title = I18n::text(QStringLiteral("page.default_file_name"));
  title.replace(QRegularExpression(QStringLiteral("[/\\\\?%*:|\"<>]")), QStringLiteral("_"));
  const QString defaultPath = QDir::homePath() + QLatin1Char('/') + title + QStringLiteral(".pdf");
  const QString filePath = QFileDialog::getSaveFileName(this,
      I18n::text(QStringLiteral("print.pdf_save_title")),
      defaultPath,
      I18n::text(QStringLiteral("print.pdf_filter")));
  if (!filePath.isEmpty()) {
    view->page()->printToPdf(filePath);
  }
}

void BrowserWindow::captureVisiblePage() {
  auto *view = currentView();
  auto *ev = currentEngineView();
  if (!view && !ev) return;
  QString title = ev ? ev->title().trimmed() : view->title().trimmed();
  if (title.isEmpty()) title = I18n::text(QStringLiteral("page.default_file_name"));
  title.replace(QRegularExpression(QStringLiteral("[/\\\\?%*:|\"<>]")), QStringLiteral("_"));
  const QString filePath = QFileDialog::getSaveFileName(
      this,
      I18n::text(QStringLiteral("capture.save_title")),
      QDir::homePath() + QLatin1Char('/') + title + QStringLiteral(".png"),
      I18n::text(QStringLiteral("capture.png_filter")));
  if (filePath.isEmpty()) return;
  QString target = filePath;
  if (!target.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) target += QStringLiteral(".png");
  const QPixmap capture = ev ? ev->grab() : view->grab();
  if (capture.isNull() || !capture.save(target, "PNG")) {
    QMessageBox::warning(this,
        I18n::text(QStringLiteral("capture.error_title")),
        I18n::text(QStringLiteral("capture.error")));
  }
}

void BrowserWindow::showReaderMode() {
  auto *sourceView = currentView();
  auto *sourceEv = currentEngineView();
  const int sourceIndex = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if ((!sourceView && !sourceEv) || sourceIndex < 0 || sourceIndex >= tabs_.size()) return;
  const QUrl sourceUrl = sourceEv ? sourceEv->url() : sourceView->url();
  if (sourceUrl.scheme() != QLatin1String("http") && sourceUrl.scheme() != QLatin1String("https")) return;
  const uint64_t sourceTabId = tabs_[sourceIndex].id;
  const QPointer<BrowserWindow> guard(this);
  const QString readerScript = QStringLiteral(R"JS((() => {
    const root = document.querySelector('article, main, [role="main"]') || document.body;
    if (!root) return null;
    const blocks = [...root.querySelectorAll('h1,h2,h3,p,blockquote,pre,li')]
      .map(node => ({tag: node.tagName.toLowerCase(), text: (node.innerText || '').replace(/\s+/g, ' ').trim()}))
      .filter(item => item.text.length >= 20).slice(0, 500);
    const length = blocks.reduce((total, item) => total + item.text.length, 0);
    if (blocks.length < 3 || length < 300) return null;
    return {title: (document.querySelector('article h1, main h1, h1')?.innerText || document.title || '').trim(),
            site: location.hostname, url: location.href, blocks};
  })())JS");

  auto callback = [guard, sourceTabId](const QVariant &value) {
    if (!guard) return;
    const QVariantMap article = value.toMap();
    const QVariantList blocks = article.value(QStringLiteral("blocks")).toList();
    if (blocks.isEmpty()) {
      QMessageBox::information(guard,
          I18n::text(QStringLiteral("reader.title")),
          I18n::text(QStringLiteral("reader.unsupported")));
      return;
    }

    auto *readerPage = new QWidget;
    readerPage->setObjectName(QStringLiteral("reader-mode-page"));
    auto *layout = new QVBoxLayout(readerPage);
    layout->setContentsMargins(56, 28, 56, 28);
    layout->setSpacing(14);
    auto *exit = new QPushButton(I18n::text(QStringLiteral("reader.exit")), readerPage);
    exit->setObjectName(QStringLiteral("reader-exit-button"));
    exit->setMaximumWidth(220);
    layout->addWidget(exit, 0, Qt::AlignLeft);
    auto *content = new QTextBrowser(readerPage);
    content->setObjectName(QStringLiteral("reader-content"));
    content->setOpenExternalLinks(true);
    content->setStyleSheet(QStringLiteral("QTextBrowser{background:#f7f3e8;color:#24211d;border:0;padding:24px;font:18px serif;}"));
    QString html = QStringLiteral("<h1>%1</h1><p><small>%2</small></p>")
        .arg(article.value(QStringLiteral("title")).toString().toHtmlEscaped(),
             article.value(QStringLiteral("site")).toString().toHtmlEscaped());
    for (const QVariant &itemValue : blocks) {
      const QVariantMap item = itemValue.toMap();
      const QString tag = item.value(QStringLiteral("tag")).toString();
      const QString text = item.value(QStringLiteral("text")).toString().left(12000).toHtmlEscaped();
      if (tag.startsWith(QLatin1Char('h'))) html += QStringLiteral("<h2>%1</h2>").arg(text);
      else if (tag == QLatin1String("blockquote")) html += QStringLiteral("<blockquote>%1</blockquote>").arg(text);
      else html += QStringLiteral("<p>%1</p>").arg(text);
    }
    content->setHtml(html);
    layout->addWidget(content, 1);
    const int readerIndex = guard->addInternalTab(readerPage,
        article.value(QStringLiteral("title")).toString().left(80),
        BrowserIcons::icon(BrowserIcon::Cards), QStringLiteral("reader-mode"));
    if (readerIndex < 0) { readerPage->deleteLater(); return; }
    const uint64_t readerTabId = guard->tabs_[readerIndex].id;
    QObject::connect(exit, &QPushButton::clicked, guard, [guard, sourceTabId, readerTabId] {
      if (!guard) return;
      const int reader = guard->findIndexByTabId(readerTabId);
      if (reader >= 0) guard->closeTab(reader);
      const int source = guard->findIndexByTabId(sourceTabId);
      if (source >= 0) guard->switchTab(source);
    });
  };

  if (sourceEv) {
    sourceEv->runJavaScript(readerScript, callback);
  } else if (sourceView && sourceView->page()) {
    sourceView->page()->runJavaScript(readerScript, callback);
  }
}

void BrowserWindow::toggleBrowserFullScreen() {
  if (isFullScreen()) {
    isWebFullScreen_ = false;
    if (topBar_) topBar_->show();
    if (navBar_) navBar_->show();
    showNormal();
    if (windowStateBeforeFullScreen_.testFlag(Qt::WindowMaximized)) showMaximized();
    else if (!geometryBeforeFullScreen_.isNull()) setGeometry(geometryBeforeFullScreen_);
    updateBookmarkBarVisibility();
    return;
  }
  windowStateBeforeFullScreen_ = windowState();
  geometryBeforeFullScreen_ = geometry();
  isWebFullScreen_ = true;
  if (topBar_) topBar_->hide();
  if (navBar_) navBar_->hide();
  if (bookmarkBar_) bookmarkBar_->hide();
  showFullScreen();
}

void BrowserWindow::handleFullScreenRequest(QWebEngineView *view, const QWebEngineFullScreenRequest &request) {
  auto req = const_cast<QWebEngineFullScreenRequest &>(request);
  req.accept();
  setPageFullScreen(req.toggleOn(), view);
}

void BrowserWindow::setPageFullScreen(bool on, QWidget *owner) {
  pageFullScreenOwner_ = on ? owner : nullptr;
  if (on) {
    if (!isWebFullScreen_) {
      windowStateBeforeFullScreen_ = windowState();
      geometryBeforeFullScreen_ = geometry();
      isWebFullScreen_ = true;
      if (topBar_) topBar_->hide();
      if (navBar_) navBar_->hide();
      if (bookmarkBar_) bookmarkBar_->hide();
      showFullScreen();
    }
  } else {
    if (isWebFullScreen_) {
      isWebFullScreen_ = false;
      if (topBar_) topBar_->show();
      if (navBar_) navBar_->show();
      updateBookmarkBarVisibility();
      const Qt::WindowStates prevState = windowStateBeforeFullScreen_ & ~Qt::WindowFullScreen;
      if (prevState.testFlag(Qt::WindowMaximized)) {
        showMaximized();
      } else {
        showNormal();
        if (!geometryBeforeFullScreen_.isNull()) {
          setGeometry(geometryBeforeFullScreen_);
        }
      }
    }
  }
}
