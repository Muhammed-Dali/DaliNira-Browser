#pragma once

#include "browser_icons.h"
#include "i18n/i18n.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMouseEvent>
#include <QPair>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QString>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif

#include <functional>

namespace dalinira::settings_ui {

constexpr int kContentMaxWidth = 920;

struct Section {
  QWidget *page = nullptr;
  QVBoxLayout *layout = nullptr;
};

class ClickableFrame final : public QFrame {
 public:
  using QFrame::QFrame;
  std::function<void()> clicked;

 protected:
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && clicked) {
      clicked();
      event->accept();
      return;
    }
    QFrame::mouseReleaseEvent(event);
  }
};

struct InteractiveSettingRowResult {
  ClickableFrame *frame = nullptr;
  QLabel *subtitleLabel = nullptr;
};

inline InteractiveSettingRowResult makeInteractiveSettingRow(
    QWidget *parent,
    BrowserIcon icon,
    const QString &title,
    const QString &initialSubtitle,
    std::function<void(QLabel *)> onClickAction) {
  auto *row = new ClickableFrame(parent);
  row->setObjectName(QStringLiteral("settings-row"));
  row->setCursor(Qt::PointingHandCursor);

  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(14);

  auto *iconLabel = new QLabel(row);
  iconLabel->setPixmap(BrowserIcons::icon(icon).pixmap(18, 18));
  iconLabel->setFixedSize(20, 20);
  layout->addWidget(iconLabel, 0, Qt::AlignVCenter);

  auto *textCol = new QWidget(row);
  auto *textLayout = new QVBoxLayout(textCol);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(3);

  auto *titleLabel = new QLabel(title, textCol);
  titleLabel->setObjectName(QStringLiteral("settings-row-title"));
  textLayout->addWidget(titleLabel);

  auto *subLabel = new QLabel(initialSubtitle, textCol);
  subLabel->setObjectName(QStringLiteral("settings-row-description"));
  subLabel->setWordWrap(true);
  textLayout->addWidget(subLabel);

  layout->addWidget(textCol, 1, Qt::AlignVCenter);

  auto *chevron = new QLabel(row);
  chevron->setPixmap(BrowserIcons::icon(BrowserIcon::ChevronRight).pixmap(14, 14));
  chevron->setFixedSize(16, 16);
  layout->addWidget(chevron, 0, Qt::AlignVCenter);

  if (onClickAction) {
    row->clicked = [onClickAction, subLabel] {
      onClickAction(subLabel);
    };
  }

  return {row, subLabel};
}

inline InteractiveSettingRowResult makeInteractiveSettingRow(
    QWidget *parent,
    BrowserIcon icon,
    const QString &title,
    const QString &initialSubtitle,
    std::function<void()> onClickAction) {
  return makeInteractiveSettingRow(parent, icon, title, initialSubtitle, [onClickAction](QLabel *) {
    if (onClickAction) onClickAction();
  });
}

struct CollapsibleSectionResult {
  ClickableFrame *headerRow = nullptr;
  QWidget *childContainer = nullptr;
};

inline CollapsibleSectionResult makeCollapsibleSectionRow(
    QWidget *parent,
    const QString &title,
    const QString &description = {}) {
  auto *header = new ClickableFrame(parent);
  header->setObjectName(QStringLiteral("settings-row"));
  header->setCursor(Qt::PointingHandCursor);

  auto *layout = new QHBoxLayout(header);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(14);

  auto *textCol = new QWidget(header);
  auto *textLayout = new QVBoxLayout(textCol);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(3);

  auto *titleLabel = new QLabel(title, textCol);
  titleLabel->setObjectName(QStringLiteral("settings-row-title"));
  textLayout->addWidget(titleLabel);

  if (!description.isEmpty()) {
    auto *descLabel = new QLabel(description, textCol);
    descLabel->setObjectName(QStringLiteral("settings-row-description"));
    textLayout->addWidget(descLabel);
  }
  layout->addWidget(textCol, 1, Qt::AlignVCenter);

  auto *arrowLabel = new QLabel(header);
  arrowLabel->setPixmap(BrowserIcons::icon(BrowserIcon::ChevronDown).pixmap(14, 14));
  arrowLabel->setFixedSize(16, 16);
  layout->addWidget(arrowLabel, 0, Qt::AlignVCenter);

  auto *container = new QWidget(parent);
  container->setVisible(false);
  auto *cLayout = new QVBoxLayout(container);
  cLayout->setContentsMargins(0, 0, 0, 0);
  cLayout->setSpacing(0);

  header->clicked = [container] {
    container->setVisible(!container->isVisible());
  };

  return {header, container};
}

inline void showOptionDialog(
    QWidget *parent,
    const QString &title,
    BrowserIcon icon,
    const QString &description,
    const QList<QPair<QString, QString>> &options,
    int initialIndex,
    std::function<void(int)> onSelect) {
  QDialog dialog(parent);
  dialog.setWindowTitle(title);
  dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
  dialog.setMinimumWidth(490);
  dialog.setStyleSheet(QStringLiteral(R"CSS(
    QDialog { background: #151d26; color: #edf5fc; border: 1px solid #2e3b49; border-radius: 12px; }
    #dialog-header { background: #1c2633; border-bottom: 1px solid #2d3b4b; border-top-left-radius: 11px; border-top-right-radius: 11px; }
    #dialog-title { font-size: 16px; font-weight: 650; color: #f2f7fc; }
    #dialog-desc { color: #8fa0b3; font-size: 13px; }
    #dialog-option-box { background: #1a232e; border: 1px solid #2d3c4c; border-radius: 10px; margin-bottom: 6px; }
    #dialog-option-box:hover { background: #222e3d; border-color: #3e566e; }
    QRadioButton { color: #edf4fb; font-size: 14px; font-weight: 550; spacing: 10px; }
    QRadioButton::indicator { width: 18px; height: 18px; border-radius: 9px; border: 2px solid #4a5c6e; background: #111820; }
    QRadioButton::indicator:checked { border-color: #55a8cf; background: #55a8cf; }
    #dialog-option-desc { color: #8899ac; font-size: 12px; margin-left: 28px; }
    QPushButton { min-height: 32px; background: #243546; color: #edf5fc; border: 1px solid #3c5164; border-radius: 7px; padding: 0 16px; font-weight: 550; }
    QPushButton:hover { background: #2d4358; border-color: #4b667e; }
    QPushButton#primary { background: #32759e; border-color: #4595c2; }
    QPushButton#primary:hover { background: #3c8bb9; }
  )CSS"));

  auto *root = new QVBoxLayout(&dialog);
  root->setContentsMargins(0, 0, 0, 16);
  root->setSpacing(14);

  auto *header = new QWidget(&dialog);
  header->setObjectName(QStringLiteral("dialog-header"));
  auto *hLayout = new QHBoxLayout(header);
  hLayout->setContentsMargins(18, 14, 18, 14);
  hLayout->setSpacing(12);

  auto *iconLbl = new QLabel(header);
  iconLbl->setPixmap(BrowserIcons::icon(icon).pixmap(22, 22));
  iconLbl->setFixedSize(24, 24);
  hLayout->addWidget(iconLbl, 0, Qt::AlignVCenter);

  auto *titleCol = new QWidget(header);
  auto *tLayout = new QVBoxLayout(titleCol);
  tLayout->setContentsMargins(0, 0, 0, 0);
  tLayout->setSpacing(2);
  auto *titleLbl = new QLabel(title, titleCol);
  titleLbl->setObjectName(QStringLiteral("dialog-title"));
  tLayout->addWidget(titleLbl);
  if (!description.isEmpty()) {
    auto *descLbl = new QLabel(description, titleCol);
    descLbl->setObjectName(QStringLiteral("dialog-desc"));
    descLbl->setWordWrap(true);
    tLayout->addWidget(descLbl);
  }
  hLayout->addWidget(titleCol, 1);
  root->addWidget(header);

  auto *body = new QWidget(&dialog);
  auto *bLayout = new QVBoxLayout(body);
  bLayout->setContentsMargins(20, 0, 20, 0);
  bLayout->setSpacing(8);

  auto *group = new QButtonGroup(&dialog);
  for (int i = 0; i < options.size(); ++i) {
    auto *optBox = new ClickableFrame(body);
    optBox->setObjectName(QStringLiteral("dialog-option-box"));
    optBox->setCursor(Qt::PointingHandCursor);
    auto *optLayout = new QVBoxLayout(optBox);
    optLayout->setContentsMargins(14, 10, 14, 10);
    optLayout->setSpacing(3);

    auto *rb = new QRadioButton(options[i].first, optBox);
    rb->setChecked(i == initialIndex);
    group->addButton(rb, i);
    optLayout->addWidget(rb);

    if (!options[i].second.isEmpty()) {
      auto *sub = new QLabel(options[i].second, optBox);
      sub->setObjectName(QStringLiteral("dialog-option-desc"));
      sub->setWordWrap(true);
      optLayout->addWidget(sub);
    }
    optBox->clicked = [rb] { rb->setChecked(true); };
    bLayout->addWidget(optBox);
  }
  root->addWidget(body);

  auto *btnLayout = new QHBoxLayout;
  btnLayout->setContentsMargins(20, 6, 20, 0);
  btnLayout->addStretch();
  auto *cancelBtn = new QPushButton(I18n::text(QStringLiteral("common.cancel")), &dialog);
  auto *saveBtn = new QPushButton(I18n::text(QStringLiteral("common.save")), &dialog);
  saveBtn->setObjectName(QStringLiteral("primary"));
  btnLayout->addWidget(cancelBtn);
  btnLayout->addWidget(saveBtn);
  root->addLayout(btnLayout);

  QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
  QObject::connect(saveBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

  if (dialog.exec() == QDialog::Accepted) {
    const int checkedId = group->checkedId();
    if (checkedId >= 0 && checkedId < options.size()) {
      onSelect(checkedId);
    }
  }
}

inline Section makeSection(const QString &title, const QString &description) {
  auto *page = new QWidget;
  page->setObjectName(QStringLiteral("settings-section"));
  auto *outer = new QHBoxLayout(page);
  outer->setContentsMargins(22, 20, 22, 32);
  outer->addStretch(1);
  auto *column = new QWidget(page);
  column->setMaximumWidth(kContentMaxWidth);
  column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto *layout = new QVBoxLayout(column);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(18);
  auto *heading = new QLabel(QStringLiteral("<h2>%1</h2><p>%2</p>").arg(title, description), column);
  heading->setWordWrap(true);
  heading->setObjectName(QStringLiteral("settings-heading"));
  layout->addWidget(heading);
  outer->addWidget(column, 2);
  outer->addStretch(1);
  return {page, layout};
}

inline QFrame *makeCard(QWidget *parent, const QString &title = {}) {
  auto *card = new QFrame(parent);
  card->setObjectName(QStringLiteral("settings-card"));
  auto *layout = new QVBoxLayout(card);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  if (!title.isEmpty()) {
    auto *label = new QLabel(title, card);
    label->setObjectName(QStringLiteral("settings-card-title"));
    layout->addWidget(label);
  }
  return card;
}

inline QVBoxLayout *cardLayout(QFrame *card) { return qobject_cast<QVBoxLayout *>(card->layout()); }

inline QWidget *settingRow(QWidget *parent, const QString &title, const QString &description,
                           QWidget *control, BrowserIcon icon = BrowserIcon::Info, bool showIcon = false) {
  auto *row = new QWidget(parent);
  row->setObjectName(QStringLiteral("settings-row"));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(14);
  if (showIcon) {
    auto *iconLabel = new QLabel(row);
    iconLabel->setPixmap(BrowserIcons::icon(icon).pixmap(18, 18));
    iconLabel->setFixedSize(20, 20);
    iconLabel->setAccessibleName(title);
    layout->addWidget(iconLabel, 0, Qt::AlignTop);
  }
  auto *text = new QWidget(row);
  auto *textLayout = new QVBoxLayout(text);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(3);
  auto *titleLabel = new QLabel(title, text);
  titleLabel->setObjectName(QStringLiteral("settings-row-title"));
  titleLabel->setWordWrap(true);
  textLayout->addWidget(titleLabel);
  if (!description.isEmpty()) {
    auto *descriptionLabel = new QLabel(description, text);
    descriptionLabel->setObjectName(QStringLiteral("settings-row-description"));
    descriptionLabel->setWordWrap(true);
    textLayout->addWidget(descriptionLabel);
  }
  layout->addWidget(text, 1);
  if (control) {
    control->setParent(row);
    layout->addWidget(control, 0, Qt::AlignVCenter);
  }
  return row;
}

inline void addRow(QFrame *card, QWidget *row) {
  QVBoxLayout *layout = cardLayout(card);
  if (layout->count() > 0) {
    auto *separator = new QFrame(card);
    separator->setObjectName(QStringLiteral("settings-row-separator"));
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);
  }
  layout->addWidget(row);
}

inline QWidget *sliderControl(QSlider **sliderOut, QLabel **valueOut, int value, QWidget *parent) {
  auto *container = new QWidget(parent);
  auto *layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  auto *slider = new QSlider(Qt::Horizontal, container);
  slider->setRange(0, 100);
  slider->setValue(value);
  slider->setMinimumWidth(150);
  auto *label = new QLabel(QStringLiteral("%1%").arg(value), container);
  label->setObjectName(QStringLiteral("settings-value"));
  label->setMinimumWidth(42);
  label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  layout->addWidget(slider);
  layout->addWidget(label);
  *sliderOut = slider;
  *valueOut = label;
  return container;
}

inline QWidget *placeholderPanel(QWidget *parent, BrowserIcon icon, const QString &title, const QString &description) {
  auto *card = makeCard(parent);
  auto *content = settingRow(card, title, description, nullptr, icon, true);
  addRow(card, content);
  return card;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
inline QString permissionText(QWebEnginePermission::PermissionType type) {
  switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture: return I18n::text(QStringLiteral("settings.site.microphone"));
    case QWebEnginePermission::PermissionType::MediaVideoCapture: return I18n::text(QStringLiteral("settings.site.camera"));
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture: return I18n::text(QStringLiteral("settings.site.camera_and_microphone"));
    case QWebEnginePermission::PermissionType::DesktopVideoCapture: return I18n::text(QStringLiteral("settings.site.screen_sharing"));
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture: return I18n::text(QStringLiteral("settings.site.screen_and_audio_sharing"));
    case QWebEnginePermission::PermissionType::Notifications: return I18n::text(QStringLiteral("settings.site.notifications"));
    case QWebEnginePermission::PermissionType::Geolocation: return I18n::text(QStringLiteral("settings.site.location"));
    case QWebEnginePermission::PermissionType::ClipboardReadWrite: return I18n::text(QStringLiteral("site.clipboard_access"));
    case QWebEnginePermission::PermissionType::LocalFontsAccess: return I18n::text(QStringLiteral("settings.site.local_fonts"));
    case QWebEnginePermission::PermissionType::MouseLock: return I18n::text(QStringLiteral("settings.site.pointer_lock"));
    default: return I18n::text(QStringLiteral("settings.site.other_permission"));
  }
}

inline QString permissionState(const QWebEnginePermission &permission) {
  switch (permission.state()) {
    case QWebEnginePermission::State::Granted: return I18n::text(QStringLiteral("settings.site.state_allowed"));
    case QWebEnginePermission::State::Denied: return I18n::text(QStringLiteral("shield.cookies_blocked"));
    default: return I18n::text(QStringLiteral("settings.site.state_ask"));
  }
}

inline BrowserIcon permissionIcon(QWebEnginePermission::PermissionType type) {
  switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture: return BrowserIcon::Microphone;
    case QWebEnginePermission::PermissionType::MediaVideoCapture: return BrowserIcon::Camera;
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture: return BrowserIcon::Camera;
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture: return BrowserIcon::Video;
    case QWebEnginePermission::PermissionType::Notifications: return BrowserIcon::Notification;
    case QWebEnginePermission::PermissionType::Geolocation: return BrowserIcon::Location;
    case QWebEnginePermission::PermissionType::ClipboardReadWrite: return BrowserIcon::Clipboard;
    case QWebEnginePermission::PermissionType::LocalFontsAccess: return BrowserIcon::Fonts;
    case QWebEnginePermission::PermissionType::MouseLock: return BrowserIcon::Mouse;
    default: return BrowserIcon::Privacy;
  }
}
#endif

inline QString settingsStyleSheet() {
  return QStringLiteral(R"CSS(
    #settings-page { background:#121820; color:#e8eef6; }
    #settings-header { background:#171e27; border-bottom:1px solid #2a3542; }
    #settings-title { color:#f3f7fc; font-size:24px; font-weight:650; }
    #settings-search { min-height:38px; background:#202a36; color:#eef5fc; border:1px solid #39495b; border-radius:19px; padding:0 14px; selection-background-color:#3c7697; }
    #settings-search:hover { border-color:#526579; }
    #settings-search:focus { background:#18222d; border:2px solid #58a6c7; padding:0 13px; }
    #settings-sidebar { background:#171e27; border:0; border-right:1px solid #2a3542; padding:10px 8px; color:#bdc9d7; outline:0; }
    #settings-sidebar::item { min-height:38px; padding:0 10px; margin:2px 0; border-radius:8px; }
    #settings-sidebar::item:hover { background:#222d39; color:#e7eef7; }
    #settings-sidebar::item:selected { background:#273948; color:#f3f8fd; border-left:3px solid #62a9c8; padding-left:7px; }
    #settings-sidebar::item:disabled { min-height:8px; max-height:8px; background:transparent; }
    #settings-section, QScrollArea, QScrollArea > QWidget > QWidget { background:#121820; border:0; }
    #settings-heading { color:#f2f6fb; }
    #settings-heading h2 { margin:0; font-size:22px; }
    #settings-heading p { margin-top:6px; color:#98a8b9; }
    #settings-card { background:#1a222c; border:1px solid #2e3b49; border-radius:12px; }
    #settings-card-title { color:#9fb0c2; font-size:12px; font-weight:650; padding:14px 18px 8px; }
    #settings-row { background:transparent; }
    #settings-row:hover { background:#1e2935; }
    #settings-row-title { color:#e7edf5; font-size:14px; font-weight:550; }
    #settings-row-description { color:#91a1b2; font-size:12px; }
    #settings-row-separator { color:#2a3542; background:#2a3542; border:0; max-height:1px; margin-left:18px; }
    #settings-value { color:#a9bacb; }
    #settings-page QPushButton { min-height:30px; background:#25384a; color:#edf5fc; border:1px solid #40576b; border-radius:7px; padding:2px 12px; }
    #settings-page QPushButton:hover { background:#2d475d; border-color:#54728a; }
    #settings-page QPushButton:focus { border:2px solid #62a9c8; padding:1px 11px; }
    #settings-page QPushButton[danger="true"] { background:#272f38; color:#d8e0e8; border-color:#4a5662; }
    #settings-page QPushButton[danger="true"]:hover { background:#343d47; }
    #settings-page QLineEdit { min-height:32px; background:#111820; color:#e6edf5; border:1px solid #3a4958; border-radius:7px; padding:0 10px; }
    #settings-page QLineEdit:focus { border:2px solid #58a6c7; padding:0 9px; }
    #settings-page QComboBox { combobox-popup: 0; min-height:36px; background:#131c26; color:#e6edf5; border:1px solid #324354; border-radius:8px; padding:2px 12px; font-size:14px; }
    #settings-page QComboBox:hover { background:#182330; border-color:#485e75; }
    #settings-page QComboBox:focus { border:2px solid #58a6c7; padding:1px 11px; }
    #settings-page QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:26px; border:none; }
    #settings-page QComboBox QAbstractItemView { background:#141c26; color:#e6edf5; border:1px solid #2f4052; border-radius:8px; padding:6px; outline:none; }
    #settings-page QComboBox QAbstractItemView::item { min-height:38px; padding:6px 12px; border-radius:6px; margin:2px 0; font-size:14px; }
    #settings-page QComboBox QAbstractItemView::item:hover { background:#1d2938; color:#ffffff; }
    #settings-page QComboBox QAbstractItemView::item:selected { background:#253a4f; color:#79c0ff; font-weight:550; }
    #settings-page QComboBox#settings-browser-engine { min-height:42px; font-size:14px; font-weight:500; padding:2px 14px; }
    #settings-page QComboBox#settings-browser-engine QAbstractItemView { background:#151d27; border:1px solid #334455; border-radius:8px; padding:6px; min-height:104px; }
    #settings-page QComboBox#settings-browser-engine QAbstractItemView::item { min-height:44px; padding:6px 14px; font-size:14px; font-weight:500; }
    #settings-page QListWidget#settings-data-list, #settings-page QListWidget#settings-allowlist-list { background:#151c24; color:#e1e8f0; border:1px solid #2e3b49; border-radius:9px; outline:0; padding:4px; }
    #settings-page QListWidget#settings-data-list::item, #settings-page QListWidget#settings-allowlist-list::item { min-height:46px; padding:5px 8px; border-radius:6px; }
    #settings-page QListWidget#settings-data-list::item:hover, #settings-page QListWidget#settings-allowlist-list::item:hover { background:#202b36; }
    #settings-page QListWidget#settings-data-list::item:selected, #settings-page QListWidget#settings-allowlist-list::item:selected { background:#294052; color:#f4f8fc; }
    #settings-page QCheckBox { spacing:8px; }
    #settings-page QCheckBox::indicator { width:18px; height:18px; }
    #settings-page QSlider::groove:horizontal { height:4px; background:#344251; border-radius:2px; }
    #settings-page QSlider::sub-page:horizontal { background:#5da6c5; border-radius:2px; }
    #settings-page QSlider::handle:horizontal { width:16px; margin:-6px 0; background:#dbeaf4; border:2px solid #4c91b1; border-radius:8px; }
    #settings-mode-card { background:#141c25; border:1px solid #2e3b49; border-radius:10px; }
    #settings-mode-card:hover { background:#1a2532; border-color:#465a6f; }
    #settings-mode-card[selected="true"] { background:#1c2d3c; border:2px solid #58a6c7; }
    #settings-mode-title { color:#f3f7fc; font-size:14px; font-weight:600; }
    #settings-mode-badge { background:#284255; color:#8cd2f4; border:1px solid #3c6580; border-radius:10px; font-size:11px; font-weight:600; padding:1px 7px; }
    #settings-mode-desc { color:#98a9ba; font-size:12px; }
    #settings-subpage-back-btn { min-width:34px; max-width:34px; min-height:34px; max-height:34px; background:transparent; border:1px solid transparent; border-radius:7px; padding:0; }
    #settings-subpage-back-btn:hover { background:#222d39; border-color:#354657; }
    #settings-subpage-del-btn { min-width:28px; max-width:28px; min-height:28px; max-height:28px; background:transparent; border:1px solid transparent; border-radius:6px; padding:0; }
    #settings-subpage-del-btn:hover { background:#382229; border-color:#5c2d38; }
    #settings-site-row { background:transparent; border-bottom:1px solid #232d39; }
    #settings-site-row:hover { background:#1e2935; }
    #settings-add-btn { min-height:28px; background:#223344; color:#edf5fc; border:1px solid #3d5368; border-radius:6px; padding:0 14px; font-weight:600; font-size:12px; }
    #settings-add-btn:hover { background:#2a4156; border-color:#506d86; }
    #settings-pill-btn { min-height:28px; background:#222a35; color:#edf5fc; border:1px solid #3d4f62; border-radius:14px; padding:0 16px; font-weight:550; font-size:12px; }
    #settings-pill-btn:hover { background:#2c3a4a; border-color:#536b84; }
    #settings-pill-btn:focus { border:1px solid #62a9c8; outline:none; }
    #settings-more-btn { min-width:28px; max-width:28px; min-height:28px; max-height:28px; background:transparent; border:0; border-radius:14px; padding:0; color:#bdc9d7; font-size:16px; font-weight:bold; }
    #settings-more-btn:hover { background:#263240; color:#f3f7fc; }
    #settings-icon-del-btn { min-width:28px; max-width:28px; min-height:28px; max-height:28px; background:transparent; border:0; border-radius:6px; padding:0; color:#8e9fae; font-size:13px; }
    #settings-icon-del-btn:hover { background:#382229; color:#f28b82; }
    #settings-chevron-btn { min-width:28px; max-width:28px; min-height:28px; max-height:28px; background:transparent; border:0; border-radius:6px; padding:0; color:#8e9fae; font-size:18px; font-weight:bold; }
    #settings-chevron-btn:hover { background:#263240; color:#f3f7fc; }
  )CSS");
}

}  // namespace dalinira::settings_ui
