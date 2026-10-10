// Auto-hiding side tab panel (BrowserLayoutController states): New Tab hold,
// collapse on websites, edge reveal, hide delay, drag/menu holds, left/right,
// fullscreen, "keep open", preference persistence and focus safety. Offscreen,
// no web engine: the panel logic is the window layer's, shared by both.
#include "layout/browser_layout.h"
#include "layout/browser_layout_controller.h"
#include "layout/browser_side_panel.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <cassert>
#include <iostream>

using namespace dalinira::layout;

namespace {

void wait(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
}

// A browser window's body: [side panel] [page stack] in one row.
struct Window {
  QWidget window;
  QWidget *body = nullptr;
  QHBoxLayout *bodyLayout = nullptr;
  BrowserSidePanel *panel = nullptr;
  QStackedWidget *pages = nullptr;
  QLineEdit *pageInput = nullptr;
  QLineEdit *address = nullptr;
  BrowserLayoutController *controller = nullptr;

  Window() {
    auto *root = new QVBoxLayout(&window);
    root->setContentsMargins(0, 0, 0, 0);
    address = new QLineEdit(&window);
    root->addWidget(address);
    body = new QWidget(&window);
    bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    panel = new BrowserSidePanel(body);
    panel->hide();
    pages = new QStackedWidget(body);
    pageInput = new QLineEdit(pages);
    pages->addWidget(pageInput);
    bodyLayout->addWidget(pages, 1);
    root->addWidget(body, 1);
    controller = new BrowserLayoutController(&window, body, bodyLayout, panel, pages, nullptr, nullptr, nullptr, &window);
    window.resize(1000, 700);
    window.move(0, 0);
    window.show();
    wait(50);
  }

  QPoint bodyPoint(int x) const { return body->mapToGlobal(QPoint(x, body->height() / 2)); }
  bool pageFillsBody() const { return pages->geometry() == body->rect(); }
  SidePanelState state() const { return controller->sidePanelState(); }
  // Event-driven: no pointer poll timer exists in any state.
  bool noPoll() const { return !window.findChild<QTimer *>(QStringLiteral("floatingPanelPointerPoll")); }
  // The pointer is synthetic here; the poll would read the real one.
  void point(int x) {
    QCursor::setPos(bodyPoint(x));
    controller->updatePanelForPointer(bodyPoint(x));
  }
};

void preference() {
  // Unset: the floating layout auto-hides, the docked layouts stay pinned.
  QSettings().remove(QStringLiteral("browser/sidePanelAutoHide"));
  assert(sidePanelAutoHideEnabled(BrowserLayoutMode::FloatingSidebar));
  assert(!sidePanelAutoHideEnabled(BrowserLayoutMode::VerticalTabsLeft));
  setSidePanelAutoHideEnabled(false);
  assert(!sidePanelAutoHideEnabled(BrowserLayoutMode::FloatingSidebar));
  setSidePanelAutoHideEnabled(true);
  assert(sidePanelAutoHideEnabled(BrowserLayoutMode::VerticalTabsRight));
  assert(QSettings().value(QStringLiteral("browser/sidePanelAutoHide")).toBool());
  QSettings().remove(QStringLiteral("browser/sidePanelAutoHide"));
  std::cout << "preference persists in the profile settings, per-layout default: ok\n";
}

void dockedWithoutAutoHide() {
  Window w;
  w.controller->setLayoutMode(BrowserLayoutMode::VerticalTabsLeft, false);
  assert(w.state() == SidePanelState::Docked);
  wait(30);
  assert(w.panel->isVisible() && w.panel->geometry().right() < w.pages->geometry().left());
  w.controller->setNewTabActive(false, false);
  assert(w.state() == SidePanelState::Docked);  // setting off: the panel stays normal
  w.point(2);
  assert(w.state() == SidePanelState::Docked);
  assert(w.noPoll());
  assert(w.controller->isPinned());
  w.controller->setLayoutMode(BrowserLayoutMode::Classic, false);
  assert(w.state() == SidePanelState::None && !w.panel->isVisible() && w.noPoll());
  std::cout << "setting off: docked panel, classic without panel: ok\n";
}

void newTabAndWebsite() {
  Window w;
  w.controller->setAutoHideEnabled(true);
  w.controller->setNewTabActive(true, false);
  w.controller->setLayoutMode(BrowserLayoutMode::VerticalTabsLeft, false);
  assert(w.state() == SidePanelState::Docked);  // New Tab: open
  // Navigating away from New Tab: still docked while the page loads ...
  w.controller->setNewTabActive(false, true);
  assert(w.state() == SidePanelState::Docked);
  // ... collapsed once it has loaded; the page takes the full width.
  QCursor::setPos(w.bodyPoint(600));
  w.controller->setNewTabActive(false, false);
  assert(w.state() == SidePanelState::Collapsed);
  wait(30);
  assert(!w.panel->isVisible() && w.pageFillsBody());
  assert(w.noPoll());
  // Back to a New Tab: open again.
  w.controller->setNewTabActive(true, false);
  assert(w.state() == SidePanelState::Docked && w.panel->isVisible());
  // Leaving New Tab while the pointer is on the panel (a tab was clicked in
  // it): it stays usable over the page rather than vanishing under the pointer.
  wait(30);
  QCursor::setPos(w.panel->mapToGlobal(w.panel->rect().center()));
  w.controller->setNewTabActive(false, false);
  assert(w.state() == SidePanelState::Revealed);
  w.point(w.panel->width() + 200);
  assert(w.state() == SidePanelState::Collapsed);
  std::cout << "New Tab open, website collapsed after load, page fills body: ok\n";
}

void edgeRevealAndHide(BrowserLayoutMode mode) {
  Window w;
  const bool left = mode != BrowserLayoutMode::VerticalTabsRight;
  w.controller->setAutoHideEnabled(true);
  w.controller->setLayoutMode(mode, false);
  w.controller->setNewTabActive(false, false);
  assert(w.state() == SidePanelState::Collapsed);
  wait(30);
  const int edge = left ? 2 : w.body->width() - 3;
  const int opposite = left ? w.body->width() - 3 : 2;
  const int middle = w.body->width() / 2;
  // Ordinary page movement and the opposite edge never reveal it.
  w.point(middle);
  w.point(opposite);
  w.point(left ? 12 : w.body->width() - 13);
  assert(w.state() == SidePanelState::Collapsed);

  w.pageInput->setFocus();
  wait(20);
  QWidget *focused = QApplication::focusWidget();
  const QRect pageBefore = w.pages->geometry();
  w.point(edge);
  assert(w.state() == SidePanelState::Revealed);
  // The pointer resting on the edge (more moves there, also mid-slide) keeps
  // it open past the hide delay.
  w.point(edge);
  wait(BrowserLayoutController::kHideDelayMs + 150);
  w.point(edge);
  assert(w.state() == SidePanelState::Revealed);
  const QRect panel = w.panel->geometry();
  assert(w.panel->isVisible() && panel.height() == w.body->height());
  assert(left ? panel.left() == 0 : panel.right() == w.body->width() - 1);
  // An overlay: the page is not relaid out, and focus stays where it was.
  assert(w.pages->geometry() == pageBefore && w.pageFillsBody());
  assert(QApplication::focusWidget() == focused);

  // On the panel: stays. Just outside it: hides after the delay, not before.
  w.point(left ? panel.center().x() : panel.center().x());
  assert(w.state() == SidePanelState::Revealed);
  const int justOutside = left ? panel.right() + 12 : panel.left() - 12;
  w.point(justOutside);
  assert(w.state() == SidePanelState::Revealed);
  wait(BrowserLayoutController::kHideDelayMs / 2);
  assert(w.state() == SidePanelState::Revealed);  // no flicker on a graze
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Collapsed);
  // Clearly away: at once.
  w.point(edge);
  assert(w.state() == SidePanelState::Revealed);
  w.point(middle);
  assert(w.state() == SidePanelState::Collapsed);
  std::cout << (left ? "left" : "right") << " edge: reveal over the page, hide after delay, focus kept: ok\n";
}

void holds() {
  Window w;
  w.controller->setAutoHideEnabled(true);
  w.controller->setLayoutMode(BrowserLayoutMode::VerticalTabsLeft, false);
  w.controller->setNewTabActive(false, false);
  const int far = w.body->width() - 100;

  // Dragging a tab: never hides; afterwards the pointer decides again.
  w.point(2);
  assert(w.state() == SidePanelState::Revealed);
  // Connected as BrowserWindow connects it.
  QObject::connect(w.panel, &BrowserSidePanel::tabDragActiveChanged, w.controller,
                   &BrowserLayoutController::setTabDragActive);
  emit w.panel->tabDragActiveChanged(true);
  w.point(far);
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Revealed);
  emit w.panel->tabDragActiveChanged(false);
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Collapsed);

  // A menu opened from the panel: stays open while it is up.
  w.point(2);
  assert(w.state() == SidePanelState::Revealed);
  QMenu menu(w.panel);
  menu.addAction(QStringLiteral("x"));
  menu.popup(w.panel->mapToGlobal(QPoint(20, 20)));
  wait(50);
  w.point(far);
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Revealed);
  menu.close();
  wait(50);
  w.point(far);
  assert(w.state() == SidePanelState::Collapsed);

  // The pin toggle: pinned stays docked whatever the pointer does; unpinned
  // auto-hides again.
  bool pinnedSignal = false;
  QObject::connect(w.controller, &BrowserLayoutController::pinnedChanged, [&](bool pinned) { pinnedSignal = pinned; });
  w.controller->setAutoHideEnabled(false);
  assert(w.controller->isPinned() && pinnedSignal && w.state() == SidePanelState::Docked);
  w.point(far);
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Docked);
  w.controller->setAutoHideEnabled(true);
  assert(!w.controller->isPinned() && !pinnedSignal && w.state() == SidePanelState::Collapsed);

  // Fullscreen: collapsed, even on New Tab; no edge reveal, no polling.
  w.controller->setNewTabActive(true, false);
  w.controller->setFullScreen(true);
  assert(w.state() == SidePanelState::Collapsed && w.noPoll());
  w.point(2);
  assert(w.state() == SidePanelState::Collapsed);
  w.controller->setFullScreen(false);
  assert(w.state() == SidePanelState::Docked);
  // Fullscreen with auto-hide off also clears the docked panel away.
  w.controller->setAutoHideEnabled(false);
  w.controller->setFullScreen(true);
  assert(w.state() == SidePanelState::Collapsed);
  w.controller->setFullScreen(false);
  assert(w.state() == SidePanelState::Docked);
  std::cout << "drag, menu, keep-open and fullscreen holds: ok\n";
}

void floatingAndAnimation() {
  Window w;
  // New Tab: the floating layout's panel is part of the window too.
  w.controller->setNewTabActive(true, false);
  w.controller->setLayoutMode(BrowserLayoutMode::FloatingSidebar, true);
  assert(w.controller->isAutoHideActive() && w.state() == SidePanelState::Docked);
  wait(30);
  assert(w.panel->isVisible() && w.panel->geometry().right() < w.pages->geometry().left());
  QCursor::setPos(w.bodyPoint(600));
  w.controller->setNewTabActive(false, false);
  assert(w.state() == SidePanelState::Collapsed);
  wait(30);
  assert(w.pageFillsBody() && w.noPoll());
  w.point(2);
  assert(w.state() == SidePanelState::Revealed);
  assert(w.panel->property("floatingCard").toBool());  // the inset, rounded card
  // A short slide: in motion now, settled after kSlideMs, then idle.
  assert(w.panel->geometry().x() != 8);
  w.point(3);  // still on the edge while it slides (left of the inset card)
  wait(BrowserLayoutController::kHideDelayMs + 150);
  assert(w.state() == SidePanelState::Revealed);  // resting on the edge keeps it
  assert(w.panel->geometry().x() == 8);
  w.point(w.body->width() - 50);
  wait(BrowserLayoutController::kSlideMs + 150);
  assert(!w.panel->isVisible());  // hidden, so not in the keyboard focus chain
  // Reduced motion (animations off): immediate.
  w.controller->setLayoutMode(BrowserLayoutMode::FloatingSidebar, false);
  w.point(2);
  assert(w.panel->geometry().x() == 8);
  w.point(w.body->width() - 50);
  assert(!w.panel->isVisible());
  std::cout << "floating layout, slide and reduced motion: ok\n";
}

// Docked beside New Tab, the panel shows the page's background through a
// tinted glass: the purple of the violet preset, darkened, never plain black
// and never the raw page colour (too transparent).
void newTabBackdrop(bool dark) {
  setPersistedAppearanceTheme(dark ? AppearanceTheme::Dark : AppearanceTheme::Light);
  Window w;
  w.controller->setNewTabActive(true, false);
  w.controller->setLayoutMode(BrowserLayoutMode::VerticalTabsLeft, false);
  w.panel->setBackdropPageArea(w.pages);
  wait(30);
  const QPoint sample(w.panel->width() - 20, w.panel->height() - 40);
  const QColor plain = w.panel->grab().toImage().pixelColor(sample);
  NewTabBackdrop violet;
  violet.valid = true;
  violet.baseColor = QColor(7, 17, 31);
  violet.cssImage = QStringLiteral("linear-gradient(135deg, rgb(37, 21, 88), rgb(125, 47, 228) 55%, rgb(218, 49, 152))");
  violet.overlay = QColor(2, 8, 18, 0);
  w.panel->setNewTabBackdrop(violet);
  wait(30);
  const QColor glass = w.panel->grab().toImage().pixelColor(sample);
  std::cout << "panel pixel plain rgb(" << plain.red() << "," << plain.green() << "," << plain.blue() << "), glass rgb("
            << glass.red() << "," << glass.green() << "," << glass.blue() << ")" << std::endl;
  // The violet shows through (blue over green), yet the glass keeps the
  // theme's contrast for the panel's text.
  assert(glass.blue() > glass.green() + 15 && glass != plain);
  if (dark) assert(glass.blue() > plain.blue() + 20 && glass.lightness() < 110);
  else assert(glass.lightness() > 150);
  w.panel->setNewTabBackdrop({});
  wait(30);
  assert(w.panel->grab().toImage().pixelColor(sample) == plain);
  std::cout << (dark ? "dark" : "light") << " theme: New Tab background through the panel's tinted glass: ok\n";
}

}  // namespace

int main(int argc, char **argv) {
  QTemporaryDir config;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTest"));
  QCoreApplication::setApplicationName(QStringLiteral("SidePanelAutoHide"));
  preference();
  dockedWithoutAutoHide();
  newTabAndWebsite();
  edgeRevealAndHide(BrowserLayoutMode::VerticalTabsLeft);
  edgeRevealAndHide(BrowserLayoutMode::VerticalTabsRight);
  holds();
  floatingAndAnimation();
  newTabBackdrop(true);
  newTabBackdrop(false);
  std::cout << "side panel auto-hide: all checks passed\n";
  return 0;
}
