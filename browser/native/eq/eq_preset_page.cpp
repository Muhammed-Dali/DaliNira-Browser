#include "eq_preset_page.h"

#include "web_audio_effects_controller.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>

#include <algorithm>
#include "i18n/i18n.h"

namespace {
QString groupName(const QString &id) {
  static const QSet<QString> known{QStringLiteral("all"), QStringLiteral("bass"), QStringLiteral("treble"),
                                   QStringLiteral("vocal"), QStringLiteral("jazz"), QStringLiteral("classical"),
                                   QStringLiteral("electronic"), QStringLiteral("pop"), QStringLiteral("rock"),
                                   QStringLiteral("vshape"), QStringLiteral("flat"), QStringLiteral("other")};
  return known.contains(id) ? I18n::text(QStringLiteral("eq.group_") + id) : id;
}

QPainterPath smoothPath(const QVector<QPointF> &points) {
  QPainterPath path;
  if (points.isEmpty()) return path;
  path.moveTo(points.first());
  for (int i = 1; i < points.size(); ++i) {
    const QPointF midpoint = (points.at(i - 1) + points.at(i)) * 0.5;
    path.quadTo(points.at(i - 1), midpoint);
  }
  path.lineTo(points.last());
  return path;
}

double limited(double value, double minimum, double maximum) {
  return std::clamp(value, minimum, maximum);
}

class PresetModel final : public QAbstractListModel {
 public:
  explicit PresetModel(EqPresetPage *page) : QAbstractListModel(page), page_(page) {}
  int rowCount(const QModelIndex &) const override { return page_->visibleForView().size(); }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= page_->visibleForView().size()) return {};
    const EqPreset &preset = page_->repositoryForView().presets().at(page_->visibleForView().at(index.row()));
    if (role == Qt::DisplayRole) return preset.name;
    if (role == Qt::UserRole) return preset.id;
    return {};
  }
  void reset() { beginResetModel(); endResetModel(); }
 private: EqPresetPage *page_;
};

class PresetDelegate final : public QStyledItemDelegate {
 public:
  explicit PresetDelegate(EqPresetPage *page) : QStyledItemDelegate(page), page_(page) {}
  QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {600, page_->rowHeightForView()}; }
  void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &index) const override {
    const EqPreset &preset = page_->repositoryForView().presets().at(page_->visibleForView().at(index.row()));
    const bool selected = preset.id == page_->selectedForView();
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    QRect r = o.rect.adjusted(5, 3, -5, -3);
    p->setPen(selected ? QColor("#00bcd4") : QColor("#20262d"));
    p->setBrush(selected ? QColor("#092529") : QColor("#07090b")); p->drawRoundedRect(r, 10, 10);
    QRect check(r.left() + 13, r.center().y() - 10, 20, 20);
    p->setPen(selected ? QColor("#29e6c4") : QColor("#63707b")); p->setBrush(Qt::NoBrush); p->drawRoundedRect(check, 5, 5);
    if (selected) { p->setPen(QPen(QColor("#29e6c4"), 2)); p->drawLine(check.left()+5, check.center().y(), check.center().x()-1, check.bottom()-5); p->drawLine(check.center().x()-1, check.bottom()-5, check.right()-4, check.top()+5); }
    QRect graph(r.left()+50, r.top()+10, 304, r.height()-20);
    const double middle = graph.center().y() + graph.height() * 0.06;
    p->setPen(QPen(QColor("#303940"), 0.9)); p->drawLine(graph.left(), middle, graph.right(), middle);
    struct CurveLayer { QColor color; double offset; double bandGain; double detailGain; double width; };
    const QVector<CurveLayer> layers{
        {QColor("#35f467"), -1.7, 1.00, 1.16, 1.28},
        {QColor("#d9ef3d"), -0.3, 0.98, 1.02, 1.13},
        {QColor("#1680ff"),  1.3, 0.92, 1.45, 1.08},
        {QColor("#c73ad9"),  2.1, 0.88, 1.52, 1.02},
        {QColor("#f6f6f6"),  2.3, 0.62, 0.82, 0.72},
    };
    const int layerCount = page_->graphLayerCountForView();
    const double minY = graph.top() + 2.0, maxY = graph.bottom() - 2.0;
    for (int curve = 0; curve < std::min(layerCount, static_cast<int>(layers.size())); ++curve) {
      const CurveLayer &layer = layers.at(curve);
      QVector<QPointF> points;
      points.reserve(preset.bands.size());
      for (int i = 0; i < preset.bands.size(); ++i) {
        const double x = graph.left() + graph.width() * i / 31.0;
        const double band = preset.bands.at(i);
        const double previous = preset.bands.at(std::max(0, i - 1));
        const double next = preset.bands.at(std::min(static_cast<int>(preset.bands.size()) - 1, i + 1));
        const double structure = ((next - previous) * 0.48) + ((next - 2.0 * band + previous) * 0.72);
        const double micro = std::sin(i * (0.57 + curve * 0.10) + curve * 1.13) * 0.68
            + std::cos(i * (1.26 + curve * 0.14) - curve * 0.71) * 0.42;
        const double highDetail = i >= 15 ? std::sin((i - 15) * (1.32 + curve * 0.13)) * 0.62 : 0.0;
        const double gain = band * layer.bandGain + structure * 2.9 * layer.detailGain
            + (micro + highDetail) * layer.detailGain;
        const double y = limited(middle - gain * graph.height() / 21.0 + layer.offset, minY, maxY);
        points.append(QPointF(x, y));
      }
      p->setPen(QPen(layer.color, layer.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p->drawPath(smoothPath(points));
    }
    QRect text(graph.right()+26, r.top()+15, r.width()-graph.width()-105, 25);
    p->setPen(QColor("#f1f5f9")); p->setFont(QFont(QString(), 10, QFont::DemiBold)); p->drawText(text, Qt::AlignVCenter, preset.name);
    p->setPen(QColor("#97a4ae")); p->setFont(QFont(QString(), 8)); p->drawText(text.adjusted(0, 24, 0, 20), Qt::AlignVCenter, preset.description);
    p->setPen(QColor("#a5b1b9")); p->setFont(QFont(QString(), 13)); p->drawText(QRect(r.right()-68, r.top(), 30, r.height()), Qt::AlignCenter, QStringLiteral("⛓"));
    p->setFont(QFont(QString(), 16)); p->drawText(QRect(r.right()-36, r.top(), 30, r.height()), Qt::AlignCenter, QStringLiteral("⋮"));
    p->restore();
  }
  bool editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    if (event->type() != QEvent::MouseButtonRelease || !index.isValid()) return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const QRect row = option.rect.adjusted(5, 3, -5, -3);
    if (mouse->position().x() < row.right() - 52) return false;
    const EqPreset &preset = page_->repositoryForView().presets().at(page_->visibleForView().at(index.row()));
    if (mouse->position().x() < row.right() - 27) {
      QApplication::clipboard()->setText(preset.name);
      return true;
    }
    QMenu menu;
    QAction *copyName = menu.addAction(I18n::text(QStringLiteral("eq.copy_preset_name")));
    QAction *copyId = menu.addAction(I18n::text(QStringLiteral("eq.copy_file_id")));
    QAction *chosen = menu.exec(mouse->globalPosition().toPoint());
    if (chosen == copyName) QApplication::clipboard()->setText(preset.name);
    if (chosen == copyId) QApplication::clipboard()->setText(preset.id);
    return chosen != nullptr;
  }
 private: EqPresetPage *page_;
};
}  // namespace

EqPresetPage::EqPresetPage(WebAudioEffectsController *controller, QWidget *parent) : QWidget(parent), controller_(controller) {
  setObjectName(QStringLiteral("eq-preset-page"));
  originalBands_ = controller_ ? controller_->equalizerBands() : QVector<double>(32, 0.0);
  repository_.load();
  const QString persistedId = QSettings().value(QStringLiteral("audioEffects/web/equalizer/presetId")).toString();
  for (const EqPreset &preset : repository_.presets()) {
    if (preset.id == persistedId && preset.bands == originalBands_) { selectedId_ = preset.id; break; }
  }
  if (selectedId_.isEmpty() && std::all_of(originalBands_.cbegin(), originalBands_.cend(), [](double value) { return value == 0.0; })) {
    selectedId_ = QStringLiteral("__flat__");
  }
  auto *layout = new QVBoxLayout(this); layout->setContentsMargins(14, 16, 14, 12); layout->setSpacing(10);
  auto *title = new QLabel(I18n::text(QStringLiteral("audio.presets")), this); title->setObjectName(QStringLiteral("eq-preset-title")); layout->addWidget(title);
  auto *rule = new QFrame(this); rule->setObjectName(QStringLiteral("eq-preset-rule")); rule->setFixedHeight(1); layout->addWidget(rule);
  search_ = new QLineEdit(this); search_->setObjectName(QStringLiteral("eq-preset-search")); search_->setPlaceholderText(I18n::text(QStringLiteral("eq.search_placeholder"))); search_->setClearButtonEnabled(true); layout->addWidget(search_);
  auto *modes = new QHBoxLayout; fullButton_ = new QPushButton(I18n::text(QStringLiteral("eq.mode_full")), this); balancedButton_ = new QPushButton(I18n::text(QStringLiteral("common.balanced")), this); minimumButton_ = new QPushButton(I18n::text(QStringLiteral("eq.mode_minimum")), this);
  for (QPushButton *button : {fullButton_, balancedButton_, minimumButton_}) { button->setCheckable(true); button->setObjectName(QStringLiteral("eq-preset-mode")); modes->addWidget(button); } modes->addStretch(); layout->addLayout(modes);
  performanceHint_ = new QLabel(this); performanceHint_->setObjectName(QStringLiteral("eq-preset-hint")); layout->addWidget(performanceHint_);
  group_ = new QComboBox(this);
  group_->setObjectName(QStringLiteral("eq-preset-group"));
  for (const QString &id : EqPresetRepository::groupOrder()) {
    int count = 0;
    for (const EqPreset &preset : repository_.presets()) {
      if (id == QLatin1String("all") || preset.groups.contains(id)) ++count;
    }
    group_->addItem(QStringLiteral("%1 (%2)").arg(groupName(id)).arg(count), id);
  }
  group_->setToolTip(I18n::text(QStringLiteral("eq.category_clears_search")));
  layout->addWidget(group_, 0, Qt::AlignLeft);
  status_ = new QLabel(this); status_->setObjectName(QStringLiteral("eq-preset-status")); layout->addWidget(status_);
  list_ = new QListView(this); list_->setObjectName(QStringLiteral("eq-preset-list")); list_->setUniformItemSizes(true); list_->setSelectionMode(QAbstractItemView::SingleSelection); list_->setItemDelegate(new PresetDelegate(this)); list_->setModel(new PresetModel(this)); layout->addWidget(list_, 1);
  auto *footer = new QHBoxLayout; footer->addStretch();
  auto *ok = new QPushButton(I18n::text(QStringLiteral("common.save")), this); ok->setObjectName(QStringLiteral("eq-preset-ok")); ok->setFixedSize(112, 36); footer->addWidget(ok); layout->addLayout(footer);
  group_->setMinimumWidth(172);
  setStyleSheet(QStringLiteral("#eq-preset-page{background:#030303}#eq-preset-title{color:#edf4f7;font-size:28px;font-weight:800}#eq-preset-rule{background:#00aeca}#eq-preset-search,#eq-preset-group{background:#090b0d;border:1px solid #2c363d;border-radius:20px;padding:10px;color:#e8f4f5;font-size:14px}#eq-preset-mode{border:1px solid #2d343a;border-radius:16px;padding:6px 14px;background:#090b0d;color:#e6edf2}#eq-preset-mode:checked{background:#083941;border-color:#18bfd0}#eq-preset-hint,#eq-preset-status{color:#a4b0b9}#eq-preset-list{background:#050607;border:1px solid #202a30;border-radius:12px}#eq-preset-ok{background:#20d7bd;color:#001b1c;font-size:14px;font-weight:800;border:0;border-radius:9px;padding:0 18px}#eq-preset-ok:hover{background:#35e4c9}#eq-preset-ok:pressed{background:#18bda7}"));
  connect(search_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
  connect(group_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
    // A device search such as "Sony" should not make musical categories look
    // empty.  Choosing a category is a new browsing action, so reveal that
    // category's complete preset collection.
    if (!search_->text().isEmpty()) {
      const QSignalBlocker blocker(search_);
      search_->clear();
    }
    applyFilter();
  });
  connect(list_, &QListView::clicked, this, &EqPresetPage::preview); connect(ok, &QPushButton::clicked, this, &EqPresetPage::commit);
  connect(fullButton_, &QPushButton::clicked, this, [this] { setPerformanceMode(0); });
  connect(balancedButton_, &QPushButton::clicked, this, [this] { setPerformanceMode(1); });
  connect(minimumButton_, &QPushButton::clicked, this, [this] { setPerformanceMode(2); });
  setPerformanceMode(QSettings().value(QStringLiteral("audioEffects/web/eqPresets/performanceMode"), 1).toInt());
  applyFilter();
}

EqPresetPage::~EqPresetPage() { rollback(); }

void EqPresetPage::applyFilter() {
  const QString needle = search_->text().trimmed().toLower(); const QString wanted = group_->currentData().toString(); visible_.clear();
  for (int i = 0; i < repository_.presets().size(); ++i) { const EqPreset &p = repository_.presets()[i]; if (wanted != QLatin1String("all") && !p.groups.contains(wanted)) continue; if (!needle.isEmpty() && !(p.name + QLatin1Char(' ') + p.description + QLatin1Char(' ') + p.id).toLower().contains(needle)) continue; visible_.append(i); }
  static_cast<PresetModel *>(list_->model())->reset(); updateStatus();
}

void EqPresetPage::preview(const QModelIndex &index) { if (!index.isValid() || !controller_) return; const EqPreset &p = repository_.presets().at(visible_.at(index.row())); selectedId_ = p.id; controller_->previewEqualizerBands(p.bands); list_->viewport()->update(); }
void EqPresetPage::commit() {
  if (!controller_ || selectedId_.isEmpty()) return;
  for (const EqPreset &preset : repository_.presets()) {
    if (preset.id != selectedId_) continue;
    controller_->commitEqualizerBands(preset.bands);
    if (controller_->persistenceEnabled()) {
      QSettings().setValue(QStringLiteral("audioEffects/web/equalizer/presetId"), selectedId_);
    }
    // Kaydet, sayfayı kapatmaz. Bundan sonraki önizlemeler kapatılırsa bu
    // son kaydedilen ayara geri dönülür.
    originalBands_ = preset.bands;
    committed_ = false;
    status_->setText(I18n::text(QStringLiteral("eq.saved")).arg(preset.name));
    return;
  }
}
void EqPresetPage::rollback() { if (!committed_ && controller_) controller_->previewEqualizerBands(originalBands_); }
void EqPresetPage::updateStatus() { status_->setText(I18n::text(QStringLiteral("eq.status")).arg(visible_.size()).arg(repository_.presets().size()).arg(groupName(group_->currentData().toString()))); }

void EqPresetPage::setPerformanceMode(int mode) {
  performanceMode_ = std::clamp(mode, 0, 2);
  graphLayerCount_ = performanceMode_ == 2 ? 2 : 5;
  rowHeight_ = performanceMode_ == 0 ? 96 : performanceMode_ == 1 ? 84 : 66;
  fullButton_->setChecked(performanceMode_ == 0);
  balancedButton_->setChecked(performanceMode_ == 1);
  minimumButton_->setChecked(performanceMode_ == 2);
  performanceHint_->setText(performanceMode_ == 0 ? I18n::text(QStringLiteral("eq.mode_full_desc"))
      : performanceMode_ == 1 ? I18n::text(QStringLiteral("eq.mode_balanced_desc"))
      : I18n::text(QStringLiteral("eq.mode_minimum_desc")));
  list_->setUniformItemSizes(performanceMode_ != 0);
  QSettings().setValue(QStringLiteral("audioEffects/web/eqPresets/performanceMode"), performanceMode_);
  if (auto *model = static_cast<PresetModel *>(list_->model())) model->reset();
}
