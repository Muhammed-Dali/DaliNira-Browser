#include "dalinira_blocker_element_picker.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_service.h"
#include "element_picker_runtime.h"
#include "i18n/i18n.h"

namespace {
constexpr int kPollIntervalMs = 150;

QString pageHost(const QUrl &pageUrl) {
  const QString scheme = pageUrl.scheme().toLower();
  if (!pageUrl.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) return {};
  // The ACE (punycode) form, as filter lists write hosts; IPv6 literals have
  // no filter-syntax form.
  const QString host = QString::fromLatin1(QUrl::toAce(pageUrl.host())).toLower();
  if (host.isEmpty() || host.contains(QLatin1Char(':'))) return {};
  return host;
}
}  // namespace

QString DaliNiraElementPickerSession::unavailableReason(const DaliNiraBlockerService *service, const QUrl &pageUrl) {
  if (!service || pageHost(pageUrl).isEmpty()) return I18n::text(QStringLiteral("blocker.picker_unavailable_page"));
  const SitePolicy policy = service->sitePolicy(pageUrl.host().toLower());
  if (!service->settings()->protectionEnabled() || policy.whitelisted || !policy.adBlocking ||
      policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch())
    return I18n::text(QStringLiteral("blocker.picker_unavailable_off"));
  return {};
}

QString DaliNiraElementPickerSession::filterFor(const QUrl &pageUrl, const QString &rawSelector, QString *error) {
  auto fail = [error]() {
    if (error) *error = I18n::text(QStringLiteral("blocker.picker_err_invalid"));
    return QString();
  };
  const QString host = pageHost(pageUrl);
  const QString selector = rawSelector.trimmed();
  if (host.isEmpty() || selector.isEmpty() || selector.size() > kMaxSelectorLength) return fail();
  static const QRegularExpression kControl(QStringLiteral("[\\x00-\\x1F\\x7F\\x{2028}\\x{2029}]"));
  if (selector.contains(kControl)) return fail();
  // No way out of the selector into a CSS rule block or at-rule, and no
  // other filter syntax (procedural, scriptlet, HTML filtering, exceptions).
  for (const char *forbidden : {"{", "}", "/*", "@import", "url(", "expression(", "##", "#@#", "#?#", "#$#", "#%#",
                                "$$", "+js("}) {
    if (selector.contains(QLatin1String(forbidden), Qt::CaseInsensitive)) return fail();
  }
  // "^..." is HTML filtering syntax (a leading "^"; "[a^=b]" is plain CSS).
  if (selector.startsWith(QLatin1Char('^'))) return fail();
  // The picker's candidates only use parentheses in :nth-of-type(n); none
  // outside quoted attribute values means no procedural operator.
  QString structure = selector;
  structure.remove(QRegularExpression(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"")));
  structure.remove(QRegularExpression(QStringLiteral(":nth-(?:of-type|child)\\(\\d+\\)")));
  if (structure.contains(QLatin1Char('(')) || structure.contains(QLatin1Char(')'))) return fail();
  const QString filter = host + QStringLiteral("##") + selector;
  if (!DaliNiraBlockerEngine::validateCustomFilterLine(filter).isEmpty()) return fail();
  return filter;
}

DaliNiraElementPickerSession::DaliNiraElementPickerSession(DaliNiraBlockerService *service, const QUrl &pageUrl,
                                                           ScriptRunner runner, QObject *parent)
    : QObject(parent), service_(service), pageUrl_(pageUrl), runner_(std::move(runner)) {
  pollTimer_.setInterval(kPollIntervalMs);
  connect(&pollTimer_, &QTimer::timeout, this, &DaliNiraElementPickerSession::poll);
}

DaliNiraElementPickerSession::~DaliNiraElementPickerSession() = default;

void DaliNiraElementPickerSession::start() {
  if (active_ || !runner_) return;
  active_ = true;
  // Localized strings reach the page only as JSON data and are shown with
  // textContent there.
  const QJsonObject strings{
      {QStringLiteral("title"), I18n::text(QStringLiteral("blocker.picker_title"))},
      {QStringLiteral("hint"), I18n::text(QStringLiteral("blocker.picker_hint"))},
      {QStringLiteral("matches"), I18n::text(QStringLiteral("blocker.picker_matches"))},
      {QStringLiteral("tooBroad"), I18n::text(QStringLiteral("blocker.picker_too_broad"))},
      {QStringLiteral("previewOn"), I18n::text(QStringLiteral("blocker.picker_preview"))},
      {QStringLiteral("previewOff"), I18n::text(QStringLiteral("blocker.picker_preview_stop"))},
      {QStringLiteral("parent"), I18n::text(QStringLiteral("blocker.picker_parent"))},
      {QStringLiteral("repick"), I18n::text(QStringLiteral("blocker.picker_repick"))},
      {QStringLiteral("create"), I18n::text(QStringLiteral("blocker.picker_create"))},
      {QStringLiteral("cancel"), I18n::text(QStringLiteral("common.cancel"))},
      {QStringLiteral("saved"), I18n::text(QStringLiteral("blocker.picker_saved"))},
      {QStringLiteral("saveFailed"), I18n::text(QStringLiteral("blocker.picker_save_failed"))},
      {QStringLiteral("undo"), I18n::text(QStringLiteral("blocker.picker_undo"))},
      {QStringLiteral("done"), I18n::text(QStringLiteral("common.close"))},
      {QStringLiteral("noCandidate"), I18n::text(QStringLiteral("blocker.picker_no_candidate"))}};
  const QJsonObject config{{QStringLiteral("strings"), strings},
                           {QStringLiteral("maxCandidates"), 6},
                           {QStringLiteral("maxSelectorLength"), kMaxSelectorLength},
                           {QStringLiteral("broadMatches"), 100}};
  QString script = QString::fromUtf8(kDaliNiraElementPickerRuntime);
  script.replace(QStringLiteral("__DALINIRA_PICKER_CONFIG__"),
                 QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
  const QPointer<DaliNiraElementPickerSession> guard(this);
  // Poll only once the picker exists, or the first poll would end the session.
  runner_(script, [guard](const QVariant &) {
    if (guard && guard->active_) guard->pollTimer_.start();
  });
}

void DaliNiraElementPickerSession::poll() {
  if (!active_ || pollPending_) return;
  pollPending_ = true;
  const QPointer<DaliNiraElementPickerSession> guard(this);
  runner_(QStringLiteral("(function(){const p=window.__daliniraPicker;return p?p.takeEvent():null;})()"),
          [guard](const QVariant &result) {
            if (!guard) return;
            guard->pollPending_ = false;
            if (!guard->active_) return;
            // No picker in the page: it closed itself (Escape, Cancel, Close)
            // or the document it was on is gone (navigation, reload, crash).
            if (!result.isValid() || result.isNull()) {
              guard->finish();
              return;
            }
            const QString event = result.toString();
            if (!event.isEmpty()) guard->handleEvent(event);
          });
}

void DaliNiraElementPickerSession::handleEvent(const QString &json) {
  if (json.size() > 4096) return;
  const QJsonObject event = QJsonDocument::fromJson(json.toUtf8()).object();
  const QString type = event.value(QStringLiteral("type")).toString();
  if (type == QLatin1String("save") && service_ && savedFilter_.isEmpty()) {
    const QString filter = filterFor(pageUrl_, event.value(QStringLiteral("selector")).toString());
    if (filter.isEmpty()) {
      runner_(QStringLiteral("window.__daliniraPicker&&window.__daliniraPicker.saved(false)"), {});
      return;
    }
    // The ordinary custom-filter list: one copy of each filter.
    QStringList filters = service_->settings()->customFilters();
    savedFilterWasNew_ = !filters.contains(filter);
    if (savedFilterWasNew_) {
      filters.append(filter);
      service_->settings()->setCustomFilters(filters);
    }
    savedFilter_ = filter;
    emit filterSaved(filter);
    runner_(QStringLiteral("window.__daliniraPicker&&window.__daliniraPicker.saved(true)"), {});
  } else if (type == QLatin1String("undo") && service_ && !savedFilter_.isEmpty()) {
    if (savedFilterWasNew_) {
      QStringList filters = service_->settings()->customFilters();
      filters.removeAll(savedFilter_);
      service_->settings()->setCustomFilters(filters);
      emit filterRemoved(savedFilter_);
    }
    savedFilter_.clear();
    runner_(QStringLiteral("window.__daliniraPicker&&window.__daliniraPicker.close('undone')"), {});
    finish();
  } else if (type == QLatin1String("cancel")) {
    finish();
  }
}

void DaliNiraElementPickerSession::cancel() {
  if (!active_) return;
  runner_(QStringLiteral("window.__daliniraPicker&&window.__daliniraPicker.close('cancel')"), {});
  finish();
}

void DaliNiraElementPickerSession::finish() {
  if (!active_) return;
  active_ = false;
  pollTimer_.stop();
  emit finished();
}
