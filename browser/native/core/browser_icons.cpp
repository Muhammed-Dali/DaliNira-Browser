#include "browser_icons.h"
#include "search_engine_definition.h"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <cmath>

namespace {
QPixmap renderTinted(const QString &path, int size, const QColor &color) {
  QSvgRenderer renderer(path);
  QPixmap source(size, size);
  source.fill(Qt::transparent);
  QPainter sourcePainter(&source);
  renderer.render(&sourcePainter, QRectF(0, 0, size, size));
  sourcePainter.end();
  QPainter tintPainter(&source);
  tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
  tintPainter.fillRect(source.rect(), color);
  return source;
}
}

QString BrowserIcons::resourcePath(BrowserIcon id) {
  switch (id) {
    case BrowserIcon::Startup: return QStringLiteral(":/browser-icons/startup.svg");
    case BrowserIcon::Appearance: return QStringLiteral(":/browser-icons/appearance.svg");
    case BrowserIcon::Performance: return QStringLiteral(":/browser-icons/performance.svg");
    case BrowserIcon::Content: return QStringLiteral(":/browser-icons/content.svg");
    case BrowserIcon::Privacy: return QStringLiteral(":/browser-icons/privacy.svg");
    case BrowserIcon::Search: return QStringLiteral(":/browser-icons/search.svg");
    case BrowserIcon::Password: return QStringLiteral(":/browser-icons/password.svg");
    case BrowserIcon::Bookmark: return QStringLiteral(":/browser-icons/bookmark.svg");
    case BrowserIcon::Download: return QStringLiteral(":/browser-icons/download.svg");
    case BrowserIcon::Language: return QStringLiteral(":/browser-icons/language.svg");
    case BrowserIcon::Translate: return QStringLiteral(":/browser-icons/language.svg");
    case BrowserIcon::Accessibility: return QStringLiteral(":/browser-icons/accessibility.svg");
    case BrowserIcon::Settings: return QStringLiteral(":/browser-icons/settings.svg");
    case BrowserIcon::Reset: return QStringLiteral(":/browser-icons/reset.svg");
    case BrowserIcon::Info: return QStringLiteral(":/browser-icons/info.svg");
    case BrowserIcon::NewTab: return QStringLiteral(":/browser-icons/new-tab.svg");
    case BrowserIcon::Window: return QStringLiteral(":/browser-icons/window.svg");
    case BrowserIcon::Incognito: return QStringLiteral(":/browser-icons/incognito.svg");
    case BrowserIcon::History: return QStringLiteral(":/browser-icons/history.svg");
    case BrowserIcon::Zoom: return QStringLiteral(":/browser-icons/zoom.svg");
    case BrowserIcon::Print: return QStringLiteral(":/browser-icons/print.svg");
    case BrowserIcon::Save: return QStringLiteral(":/browser-icons/save.svg");
    case BrowserIcon::Tools: return QStringLiteral(":/browser-icons/tools.svg");
    case BrowserIcon::Help: return QStringLiteral(":/browser-icons/help.svg");
    case BrowserIcon::Exit: return QStringLiteral(":/browser-icons/exit.svg");
    case BrowserIcon::Folder: return QStringLiteral(":/browser-icons/folder.svg");
    case BrowserIcon::Trash: return QStringLiteral(":/browser-icons/trash.svg");
    case BrowserIcon::Grid: return QStringLiteral(":/browser-icons/grid.svg");
    case BrowserIcon::Clock: return QStringLiteral(":/browser-icons/clock.svg");
    case BrowserIcon::Cards: return QStringLiteral(":/browser-icons/cards.svg");
    case BrowserIcon::Video: return QStringLiteral(":/browser-icons/video.svg");
    case BrowserIcon::Music: return QStringLiteral(":/browser-icons/music.svg");
    case BrowserIcon::Play: return QStringLiteral(":/browser-icons/play.svg");
    case BrowserIcon::Clipboard: return QStringLiteral(":/browser-icons/clipboard.svg");
    case BrowserIcon::More: return QStringLiteral(":/browser-icons/more.svg");
    case BrowserIcon::Minimize: return QStringLiteral(":/browser-icons/minimize.svg");
    case BrowserIcon::Maximize: return QStringLiteral(":/browser-icons/maximize.svg");
    case BrowserIcon::Restore: return QStringLiteral(":/browser-icons/restore.svg");
    case BrowserIcon::Close: return QStringLiteral(":/browser-icons/close.svg");
    case BrowserIcon::Audio: return QStringLiteral(":/browser-icons/audio.svg");
    case BrowserIcon::Memory: return QStringLiteral(":/browser-icons/memory.svg");
    case BrowserIcon::Location: return QStringLiteral(":/browser-icons/location.svg");
    case BrowserIcon::Camera: return QStringLiteral(":/browser-icons/camera.svg");
    case BrowserIcon::Microphone: return QStringLiteral(":/browser-icons/microphone.svg");
    case BrowserIcon::Notification: return QStringLiteral(":/browser-icons/notification.svg");
    case BrowserIcon::Cookie: return QStringLiteral(":/browser-icons/cookie.svg");
    case BrowserIcon::Javascript: return QStringLiteral(":/browser-icons/javascript.svg");
    case BrowserIcon::Image: return QStringLiteral(":/browser-icons/image.svg");
    case BrowserIcon::Popup: return QStringLiteral(":/browser-icons/popup.svg");
    case BrowserIcon::ChevronRight: return QStringLiteral(":/browser-icons/chevron-right.svg");
    case BrowserIcon::ChevronUp: return QStringLiteral(":/browser-icons/chevron-up.svg");
    case BrowserIcon::FindInPage: return QStringLiteral(":/browser-icons/find-in-page.svg");
    case BrowserIcon::ChevronDown: return QStringLiteral(":/browser-icons/chevron-down.svg");
    case BrowserIcon::Fonts: return QStringLiteral(":/browser-icons/fonts.svg");
    case BrowserIcon::Mouse: return QStringLiteral(":/browser-icons/mouse.svg");
    case BrowserIcon::Pdf: return QStringLiteral(":/browser-icons/pdf.svg");
    case BrowserIcon::LocationSlash: return QStringLiteral(":/browser-icons/location-slash.svg");
    case BrowserIcon::CameraSlash: return QStringLiteral(":/browser-icons/camera-slash.svg");
    case BrowserIcon::MicrophoneSlash: return QStringLiteral(":/browser-icons/microphone-slash.svg");
    case BrowserIcon::NotificationSlash: return QStringLiteral(":/browser-icons/notification-slash.svg");
    case BrowserIcon::ArrowLeft: return QStringLiteral(":/browser-icons/arrow-left.svg");
    case BrowserIcon::ArrowRight: return {};
    case BrowserIcon::Reload: return {};
    case BrowserIcon::Stop: return QStringLiteral(":/browser-icons/close.svg");
    case BrowserIcon::JavascriptSlash: return QStringLiteral(":/browser-icons/javascript-slash.svg");
    case BrowserIcon::ImageSlash: return QStringLiteral(":/browser-icons/image-slash.svg");
    case BrowserIcon::PopupSlash: return QStringLiteral(":/browser-icons/popup-slash.svg");
    case BrowserIcon::ProtectedContent: return QStringLiteral(":/browser-icons/protected-content.svg");
    case BrowserIcon::InsecureContent: return QStringLiteral(":/browser-icons/insecure-content.svg");
    case BrowserIcon::SiteData: return QStringLiteral(":/browser-icons/site-data.svg");
    case BrowserIcon::JsOptimize: return QStringLiteral(":/browser-icons/js-optimize.svg");
    case BrowserIcon::Fullscreen: return QStringLiteral(":/browser-icons/fullscreen.svg");
    case BrowserIcon::Tune: return QStringLiteral(":/browser-icons/tune.svg");
    case BrowserIcon::Mail: return QStringLiteral(":/browser-icons/mail.svg");
    case BrowserIcon::Keyboard: return QStringLiteral(":/browser-icons/keyboard.svg");
  }
  return {};
}

QIcon BrowserIcons::backIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 40, 48, 64}) {
    const auto renderForColor = [size](const QColor &color) {
      const qreal S = size;
      const qreal stroke = std::max(1.5, S * (2.0 / 24.0));
      QPixmap pm(size, size);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      QPen pen(color);
      pen.setWidthF(stroke);
      pen.setCapStyle(Qt::RoundCap);
      pen.setJoinStyle(Qt::RoundJoin);
      p.setPen(pen);

      const qreal cx = S * 0.5;
      const qreal cy = S * 0.5;
      p.drawLine(QPointF(cx + S * 0.28, cy), QPointF(cx - S * 0.26, cy));
      QPainterPath chevron;
      chevron.moveTo(cx - S * 0.02, cy - S * 0.25);
      chevron.lineTo(cx - S * 0.26, cy);
      chevron.lineTo(cx - S * 0.02, cy + S * 0.25);
      p.drawPath(chevron);
      return pm;
    };

    icon.addPixmap(renderForColor(QColor(QStringLiteral("#e8eaed"))), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Active, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Selected, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#687584"))), QIcon::Disabled, QIcon::Off);
  }
  return icon;
}

QIcon BrowserIcons::forwardIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 40, 48, 64}) {
    const auto renderForColor = [size](const QColor &color) {
      const qreal S = size;
      const qreal stroke = std::max(1.5, S * (2.0 / 24.0));
      QPixmap pm(size, size);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      QPen pen(color);
      pen.setWidthF(stroke);
      pen.setCapStyle(Qt::RoundCap);
      pen.setJoinStyle(Qt::RoundJoin);
      p.setPen(pen);

      const qreal cx = S * 0.5;
      const qreal cy = S * 0.5;
      p.drawLine(QPointF(cx - S * 0.28, cy), QPointF(cx + S * 0.26, cy));
      QPainterPath chevron;
      chevron.moveTo(cx + S * 0.02, cy - S * 0.25);
      chevron.lineTo(cx + S * 0.26, cy);
      chevron.lineTo(cx + S * 0.02, cy + S * 0.25);
      p.drawPath(chevron);
      return pm;
    };

    icon.addPixmap(renderForColor(QColor(QStringLiteral("#e8eaed"))), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Active, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Selected, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#687584"))), QIcon::Disabled, QIcon::Off);
  }
  return icon;
}

QIcon BrowserIcons::reloadIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 40, 48, 64}) {
    const auto renderForColor = [size](const QColor &color) {
      const qreal S = size;
      const qreal stroke = std::max(1.5, S * (2.0 / 24.0));
      QPixmap pm(size, size);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);

      const qreal cx = S * 0.5;
      const qreal cy = S * 0.5;
      const qreal r  = S * 0.29;
      const QRectF bounds(cx - r, cy - r, 2 * r, 2 * r);

      // --- Arc ---
      // Qt drawArc: 0° = 3 o'clock, angles increase CCW, negative span = CW sweep.
      // Start at 90° (12 o'clock), sweep 295° clockwise → ends at ~10-11 o'clock.
      // The gap (≈65°) sits at the top of the circle where the arrowhead will be.
      QPen pen(color);
      pen.setWidthF(stroke);
      pen.setCapStyle(Qt::RoundCap);
      pen.setJoinStyle(Qt::RoundJoin);
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawArc(bounds, 90 * 16, -295 * 16);

      // --- Arrowhead ---
      // Position: arc START = 90° in Qt = 12 o'clock = (cx, cy - r)
      // CW tangent formula at Qt angle θ (derived from d/dθ of circle pos, negated):
      //   t = (sin(θ), cos(θ))  in screen coords (Y-down)
      // Verified:  θ=90°  → (1,  0) = rightward  ✓ (CW at 12 o'clock)
      //            θ=0°   → (0,  1) = downward    ✓ (CW at  3 o'clock)
      //            θ=270° → (-1, 0) = leftward    ✓ (CW at  6 o'clock)
      //            θ=180° → (0, -1) = upward      ✓ (CW at  9 o'clock)
      const qreal arrowRad = 90.0 * M_PI / 180.0;  // 12 o'clock

      const qreal tipX = cx + r * std::cos(arrowRad);       // = cx
      const qreal tipY = cy - r * std::sin(arrowRad);       // = cy - r  (top)

      // CW tangent: (sin θ, cos θ) in screen coords
      const qreal tx = std::sin(arrowRad);   //  1.0  (rightward)
      const qreal ty = std::cos(arrowRad);   //  0.0

      // Inward normal (toward center, perpendicular to tangent, rotated 90° CCW from tangent):
      // n = (-cos θ, sin θ)
      const qreal nx = -std::cos(arrowRad);  //  0.0
      const qreal ny =  std::sin(arrowRad);  //  1.0  (downward = toward center from top)

      // Triangle: tip points in CW direction, base extends behind + sideways
      const qreal ahead = S * 0.145;   // depth (along tangent, behind tip)
      const qreal aside = S * 0.090;   // half-width (perpendicular)

      QPointF pt0(tipX, tipY);
      QPointF pt1(tipX - tx * ahead + nx * aside, tipY - ty * ahead + ny * aside);
      QPointF pt2(tipX - tx * ahead - nx * aside, tipY - ty * ahead - ny * aside);

      p.setPen(Qt::NoPen);
      p.setBrush(color);
      QPolygonF arrow;
      arrow << pt0 << pt1 << pt2;
      p.drawPolygon(arrow);

      return pm;
    };

    icon.addPixmap(renderForColor(QColor(QStringLiteral("#e8eaed"))), QIcon::Normal,   QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Active,   QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Selected, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#687584"))), QIcon::Disabled, QIcon::Off);
  }
  return icon;
}


QIcon BrowserIcons::stopIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 40, 48, 64}) {
    const auto renderForColor = [size](const QColor &color) {
      const qreal S = size;
      const qreal stroke = std::max(1.5, S * (2.0 / 24.0));
      QPixmap pm(size, size);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      QPen pen(color);
      pen.setWidthF(stroke);
      pen.setCapStyle(Qt::RoundCap);
      pen.setJoinStyle(Qt::RoundJoin);
      p.setPen(pen);

      const qreal cx = S * 0.5;
      const qreal cy = S * 0.5;
      const qreal half = S * 0.20;
      p.drawLine(QPointF(cx - half, cy - half), QPointF(cx + half, cy + half));
      p.drawLine(QPointF(cx + half, cy - half), QPointF(cx - half, cy + half));
      return pm;
    };

    icon.addPixmap(renderForColor(QColor(QStringLiteral("#e8eaed"))), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Active, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#ffffff"))), QIcon::Selected, QIcon::Off);
    icon.addPixmap(renderForColor(QColor(QStringLiteral("#687584"))), QIcon::Disabled, QIcon::Off);
  }
  return icon;
}

QIcon BrowserIcons::icon(BrowserIcon id) {
  switch (id) {
    case BrowserIcon::ArrowLeft: return backIcon();
    case BrowserIcon::ArrowRight: return forwardIcon();
    case BrowserIcon::Reload: return reloadIcon();
    case BrowserIcon::Stop: return stopIcon();
    default: break;
  }
  QIcon result;
  const QString path = resourcePath(id);
  for (const int size : {16, 18, 20, 24, 32, 36, 48, 64}) {
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#b8c5d6"))), QIcon::Normal, QIcon::Off);
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#eff7ff"))), QIcon::Active, QIcon::Off);
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#eff7ff"))), QIcon::Selected, QIcon::Off);
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#687584"))), QIcon::Disabled, QIcon::Off);
  }
  return result;
}

QIcon BrowserIcons::tintedIcon(BrowserIcon id, const QColor &normal, const QColor &active,
                               const QColor &disabled) {
  QIcon result;
  const QString path = resourcePath(id);
  for (const int size : {16, 18, 20, 24, 32, 48}) {
    result.addPixmap(renderTinted(path, size, normal), QIcon::Normal, QIcon::Off);
    result.addPixmap(renderTinted(path, size, active), QIcon::Active, QIcon::Off);
    result.addPixmap(renderTinted(path, size, active), QIcon::Selected, QIcon::Off);
    result.addPixmap(renderTinted(path, size, disabled), QIcon::Disabled, QIcon::Off);
  }
  return result;
}

QIcon BrowserIcons::appIcon() {
  const QIcon windowIcon = QGuiApplication::windowIcon();
  if (!windowIcon.isNull()) return windowIcon;
  QIcon icon(QStringLiteral(":/assets/icons/dalinira-browser-256.png"));
  if (icon.isNull()) icon = QIcon(QStringLiteral(":/icons/dalinira-browser-256.png"));
  if (icon.isNull()) icon = QIcon(QStringLiteral(":/assets/icons/dalinira-browser-128.png"));
  return icon;
}

QIcon BrowserIcons::incognitoIcon() {
  QIcon result;
  const QString path = resourcePath(BrowserIcon::Incognito);
  for (const int size : {16, 18, 20, 24, 32, 36, 48, 64}) {
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#ffffff"))), QIcon::Normal, QIcon::Off);
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#eff7ff"))), QIcon::Selected, QIcon::Off);
    result.addPixmap(renderTinted(path, size, QColor(QStringLiteral("#8899aa"))), QIcon::Disabled, QIcon::Off);
  }
  return result;
}

QIcon BrowserIcons::youtubeIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 48, 64}) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Red rounded background
    const qreal r = size * 0.22;
    const QRectF rect(0, size * 0.16, size, size * 0.68);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QStringLiteral("#ff0033")));
    p.drawRoundedRect(rect, r, r);

    // White play triangle
    QPainterPath path;
    const qreal cx = size * 0.50;
    const qreal cy = size * 0.50;
    const qreal h = size * 0.20;
    path.moveTo(cx - h * 0.7, cy - h);
    path.lineTo(cx + h * 0.9, cy);
    path.lineTo(cx - h * 0.7, cy + h);
    path.closeSubpath();
    p.setBrush(Qt::white);
    p.drawPath(path);

    icon.addPixmap(pm);
  }
  return icon;
}

QIcon BrowserIcons::youtubeMusicIcon() {
  QIcon icon;
  for (const int size : {16, 18, 20, 24, 32, 36, 48, 64}) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Red circle
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QStringLiteral("#ff0033")));
    p.drawEllipse(QRectF(0, 0, size, size));

    // Outer white concentric ring
    QPen ringPen(QColor(255, 255, 255, 200), std::max(1.0, size * 0.08));
    p.setPen(ringPen);
    p.setBrush(Qt::NoBrush);
    const qreal m = size * 0.18;
    p.drawEllipse(QRectF(m, m, size - 2 * m, size - 2 * m));

    // Inner white triangle
    QPainterPath path;
    const qreal cx = size * 0.52;
    const qreal cy = size * 0.50;
    const qreal h = size * 0.18;
    path.moveTo(cx - h * 0.6, cy - h);
    path.lineTo(cx + h * 0.8, cy);
    path.lineTo(cx - h * 0.6, cy + h);
    path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawPath(path);

    icon.addPixmap(pm);
  }
  return icon;
}

QIcon BrowserIcons::searchEngineIcon(const QString &engineName) {
  QIcon engineIcon(dalinira::core::searchEngineResourcePath(engineName));
  if (!engineIcon.isNull()) return engineIcon;

  return icon(BrowserIcon::Search);
}

QIcon BrowserIcons::combinedMediaCaptureIcon() {
  QIcon result;
  for (const int size : {16, 18, 20, 24, 32, 48}) {
    QPixmap px(size, size);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    const int subSize = size * 11 / 20;
    QPixmap cam = renderTinted(resourcePath(BrowserIcon::Camera), subSize, QColor(QStringLiteral("#58a6c7")));
    QPixmap mic = renderTinted(resourcePath(BrowserIcon::Microphone), subSize, QColor(QStringLiteral("#58a6c7")));
    p.drawPixmap(0, (size - subSize) / 2, cam);
    p.drawPixmap(size - subSize, (size - subSize) / 2, mic);
    p.end();
    result.addPixmap(px);
  }
  return result;
}

QIcon BrowserIcons::chromiumIcon() {
  static const QStringList candidates = {
      QStringLiteral(":/browser-icons/chromium.svg"),
      QStringLiteral("browser/assets/browser-icons/chromium.svg")
  };
  for (const auto &path : candidates) {
    if (QFile::exists(path)) {
      QSvgRenderer renderer(path);
      if (renderer.isValid()) {
        QIcon icon;
        for (const int size : {16, 18, 20, 24, 32, 48, 64}) {
          QPixmap pm(size, size);
          pm.fill(Qt::transparent);
          QPainter p(&pm);
          p.setRenderHint(QPainter::Antialiasing);
          p.setRenderHint(QPainter::SmoothPixmapTransform);
          renderer.render(&p);
          icon.addPixmap(pm);
        }
        return icon;
      }
    }
  }
  return icon(BrowserIcon::Startup);
}

QIcon BrowserIcons::webkitIcon() {
  static const QStringList candidates = {
      QStringLiteral(":/browser-icons/webkit.svg"),
      QStringLiteral("browser/assets/browser-icons/webkit.svg")
  };
  for (const auto &path : candidates) {
    if (QFile::exists(path)) {
      QSvgRenderer renderer(path);
      if (renderer.isValid()) {
        QIcon icon;
        for (const int size : {16, 18, 20, 24, 32, 48, 64}) {
          QPixmap pm(size, size);
          pm.fill(Qt::transparent);
          QPainter p(&pm);
          p.setRenderHint(QPainter::Antialiasing);
          p.setRenderHint(QPainter::SmoothPixmapTransform);
          renderer.render(&p);
          icon.addPixmap(pm);
        }
        return icon;
      }
    }
  }
  return icon(BrowserIcon::Startup);
}
