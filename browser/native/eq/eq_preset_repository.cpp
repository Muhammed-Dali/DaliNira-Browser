#include "eq_preset_repository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include "i18n/i18n.h"

namespace {
constexpr int kBandCount = 32;
constexpr double kMinDb = -12.0;
constexpr double kMaxDb = 12.0;

QString presetRoot() {
  const QDir executable(QCoreApplication::applicationDirPath());
  const QString local = executable.filePath(QStringLiteral("eq-presets"));
  if (QFile::exists(local)) return local;
  const QString share1 = executable.filePath(QStringLiteral("../share/dalinira-browser/eq-presets"));
  if (QFile::exists(share1)) return share1;
  const QString share2 = executable.filePath(QStringLiteral("../../share/dalinira-browser/eq-presets"));
  if (QFile::exists(share2)) return share2;
  const QString sysShare = QStringLiteral("/usr/share/dalinira-browser/eq-presets");
  if (QFile::exists(sysShare)) return sysShare;
  const QString localShare = QStringLiteral("/usr/local/share/dalinira-browser/eq-presets");
  if (QFile::exists(localShare)) return localShare;
  return share1;
}

QString normalizedText(const EqPreset &preset) {
  return (preset.id + QLatin1Char(' ') + preset.name + QLatin1Char(' ') + preset.description).toLower();
}
}  // namespace

QStringList EqPresetRepository::groupOrder() {
  return {QStringLiteral("all"), QStringLiteral("bass"), QStringLiteral("treble"), QStringLiteral("vocal"),
          QStringLiteral("jazz"), QStringLiteral("classical"), QStringLiteral("electronic"), QStringLiteral("pop"),
          QStringLiteral("rock"), QStringLiteral("vshape"), QStringLiteral("flat"), QStringLiteral("other")};
}

QVector<double> EqPresetRepository::normalizedBands(const QJsonArray &values, bool *valid) {
  QVector<double> result(kBandCount, 0.0);
  *valid = values.size() == kBandCount;
  if (!*valid) return {};
  for (int i = 0; i < kBandCount; ++i) {
    const double value = values.at(i).toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(value)) { *valid = false; return {}; }
    result[i] = std::clamp(value, kMinDb, kMaxDb);
  }
  return result;
}

QVector<double> EqPresetRepository::bandsFromPoints(const QJsonArray &points, bool *valid) {
  struct Point { int index; double value; };
  QVector<Point> source;
  for (const QJsonValue &entry : points) {
    const QJsonObject point = entry.toObject();
    const double value = point.value(QStringLiteral("v")).toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!point.contains(QStringLiteral("i")) || !std::isfinite(value)) continue;
    source.append({std::clamp(point.value(QStringLiteral("i")).toInt(), 0, kBandCount - 1), std::clamp(value, kMinDb, kMaxDb)});
  }
  std::sort(source.begin(), source.end(), [](const Point &a, const Point &b) { return a.index < b.index; });
  *valid = !source.isEmpty();
  if (!*valid) return {};
  QVector<double> result(kBandCount, source.front().value);
  for (int segment = 0; segment + 1 < source.size(); ++segment) {
    const Point a = source.at(segment), b = source.at(segment + 1);
    const int span = std::max(1, b.index - a.index);
    for (int i = a.index; i <= b.index; ++i) result[i] = a.value + (b.value - a.value) * (i - a.index) / span;
  }
  for (int i = source.back().index; i < kBandCount; ++i) result[i] = source.back().value;
  return result;
}

QStringList EqPresetRepository::groupsFor(const EqPreset &preset) {
  const QString text = normalizedText(preset);
  QSet<QString> groups;
  const auto has = [&text](const QString &needle) { return text.contains(needle); };
  if (has(QStringLiteral("jazz")) || has(QStringLiteral("caz"))) groups.insert(QStringLiteral("jazz"));
  if (has(QStringLiteral("classical")) || has(QStringLiteral("orchestra"))) groups.insert(QStringLiteral("classical"));
  if (has(QStringLiteral("electronic")) || has(QStringLiteral("edm")) || has(QStringLiteral("dance")) || has(QStringLiteral("techno"))) groups.insert(QStringLiteral("electronic"));
  if (has(QStringLiteral("pop"))) groups.insert(QStringLiteral("pop"));
  if (has(QStringLiteral("rock")) || has(QStringLiteral("metal")) || has(QStringLiteral("guitar"))) groups.insert(QStringLiteral("rock"));
  if (has(QStringLiteral("v-shape")) || has(QStringLiteral("vshape"))) groups.insert(QStringLiteral("vshape"));
  if (has(QStringLiteral("vocal")) || has(QStringLiteral("voice")) || has(QStringLiteral("speech"))) groups.insert(QStringLiteral("vocal"));
  if (has(QStringLiteral("bass")) || has(QStringLiteral("sub-bass")) || has(QStringLiteral("low end")) || has(QStringLiteral("xbass")) || has(QStringLiteral("bass boost"))) groups.insert(QStringLiteral("bass"));
  if (has(QStringLiteral("treble")) || has(QStringLiteral("bright")) || has(QStringLiteral("sparkle")) || has(QStringLiteral("air")) || has(QStringLiteral("high boost"))) groups.insert(QStringLiteral("treble"));
  if (has(QStringLiteral("flat")) || has(QStringLiteral("neutral")) || has(QStringLiteral("reference")) || has(QStringLiteral("default"))) groups.insert(QStringLiteral("flat"));
  double low = 0, mid = 0, high = 0, absMax = 0;
  for (int i = 0; i < kBandCount; ++i) {
    (i < 10 ? low : i < 22 ? mid : high) += preset.bands.value(i);
    absMax = std::max(absMax, std::abs(preset.bands.value(i)));
  }
  low /= 10; mid /= 12; high /= 10;
  if (absMax <= 0.6) groups.insert(QStringLiteral("flat"));
  if (low - mid >= 1.2 || low >= 1.0) groups.insert(QStringLiteral("bass"));
  if (high - mid >= 1.2 || high >= 1.0) groups.insert(QStringLiteral("treble"));
  if (mid - ((low + high) / 2.0) >= 1.0 && mid >= 0.8) groups.insert(QStringLiteral("vocal"));
  if (low >= .9 && high >= .9 && mid <= -.4) groups.insert(QStringLiteral("vshape"));
  if (groups.isEmpty()) groups.insert(QStringLiteral("other"));
  QStringList ordered;
  for (const QString &group : groupOrder()) if (group != QLatin1String("all") && groups.contains(group)) ordered.append(group);
  return ordered;
}

bool EqPresetRepository::load() {
  presets_.clear(); invalidCount_ = 0; dataPath_ = presetRoot();
  QVector<EqPreset> featuredPresets;
  EqPreset flatPreset;
  flatPreset.id = QStringLiteral("__flat__");
  flatPreset.name = I18n::text(QStringLiteral("eq.flat_preset_name"));
  flatPreset.description = I18n::text(QStringLiteral("eq.flat_preset_desc"));
  flatPreset.groups = {QStringLiteral("flat")};
  flatPreset.bands = QVector<double>(kBandCount, 0.0);
  featuredPresets.append(flatPreset);
  const QDir autoEq(QDir(dataPath_).filePath(QStringLiteral("autoeq")));
  const QStringList files = autoEq.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const QString &fileName : files) {
    QFile file(autoEq.filePath(fileName));
    if (!file.open(QIODevice::ReadOnly)) { ++invalidCount_; continue; }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    bool valid = false;
    EqPreset preset{fileName, object.value(QStringLiteral("name")).toString().trimmed(), object.value(QStringLiteral("description")).toString(), {}, normalizedBands(object.value(QStringLiteral("bands")).toArray(), &valid)};
    if (!valid) { ++invalidCount_; continue; }
    if (preset.name.isEmpty()) preset.name = fileName.left(fileName.size() - 5).replace(QLatin1Char('_'), QLatin1Char(' '));
    preset.groups = groupsFor(preset); presets_.append(std::move(preset));
  }
  QFile featured(QDir(dataPath_).filePath(QStringLiteral("dalinira_presets.json")));
  if (featured.open(QIODevice::ReadOnly)) {
    for (const QJsonValue &entry : QJsonDocument::fromJson(featured.readAll()).object().value(QStringLiteral("presets")).toArray()) {
      const QJsonObject object = entry.toObject(); bool valid = false;
      EqPreset preset{object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("name")).toString(), object.value(QStringLiteral("description")).toString(), {}, bandsFromPoints(object.value(QStringLiteral("points")).toArray(), &valid)};
      if (!valid) { ++invalidCount_; continue; }
      preset.groups = groupsFor(preset); featuredPresets.append(std::move(preset));
    }
  }
  std::sort(presets_.begin(), presets_.end(), [](const EqPreset &a, const EqPreset &b) { return a.name.localeAwareCompare(b.name) < 0; });
  featuredPresets += presets_;
  presets_ = std::move(featuredPresets);
  return !presets_.isEmpty();
}
