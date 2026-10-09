#include "page_translator.h"
#include "language_detector.h"
#include "translate_service.h"
#include "translate_engine_script.h"
#include "engine/engine_view.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include "i18n/i18n.h"

namespace {
constexpr int kMaxBatchSize = 40;
constexpr int kMutationPollIntervalMs = 400;

QVariantMap resultMap(const QVariant &result) {
  if (result.metaType().id() == QMetaType::QString) {
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(result.toString().toUtf8(), &error);
    return error.error == QJsonParseError::NoError && doc.isObject()
        ? doc.object().toVariantMap() : QVariantMap{};
  }
  return result.toMap();
}
}

PageTranslator::PageTranslator(QWebEngineView *view, TranslateService *service, QObject *parent)
    : QObject(parent ? parent : static_cast<QObject *>(view)), view_(view), service_(service) {
  if (service_ && !service_->defaultTargetLanguage().isEmpty())
    targetLanguage_ = service_->defaultTargetLanguage();
}

PageTranslator::PageTranslator(dalinira::engine::EngineView *view,
                               TranslateService *service, QObject *parent)
    : QObject(parent ? parent : static_cast<QObject *>(view)), engineView_(view), service_(service) {
  if (service_ && !service_->defaultTargetLanguage().isEmpty())
    targetLanguage_ = service_->defaultTargetLanguage();
}

PageTranslator::~PageTranslator() {
  stopDynamicMutationWatcher();
}

PageTranslator::State PageTranslator::state() const {
  return state_;
}

QString PageTranslator::sourceLanguage() const {
  return sourceLanguage_;
}

QString PageTranslator::targetLanguage() const {
  return targetLanguage_;
}

QString PageTranslator::lastError() const {
  return lastError_;
}

uint64_t PageTranslator::currentGeneration() const {
  return currentGeneration_;
}

void PageTranslator::ensureScriptInjected() {
  if (!hasPage()) return;
  runJavaScript(translateEngineScript());
}

bool PageTranslator::hasPage() const {
  return (view_ && view_->page()) || engineView_;
}

QUrl PageTranslator::pageUrl() const {
  return view_ ? view_->url() : (engineView_ ? engineView_->url() : QUrl{});
}

void PageTranslator::runJavaScript(
    const QString &script, std::function<void(const QVariant &)> callback) {
  if (view_ && view_->page()) {
    if (callback) view_->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld, std::move(callback));
    else view_->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld);
  } else if (engineView_) {
    engineView_->runJavaScript(script, std::move(callback));
  } else if (callback) {
    callback(QVariant{});
  }
}

void PageTranslator::detectLanguage() {
  if (!hasPage()) return;

  const QUrl url = pageUrl();
  if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
    reset();
    return;
  }

  ensureScriptInjected();

  const uint64_t generation = currentGeneration_;
  QPointer<PageTranslator> guardedThis(this);
  runJavaScript(
      QStringLiteral("window.__daliniraTranslate ? JSON.stringify(window.__daliniraTranslate.detect()) : null;"),
      [guardedThis, generation](const QVariant &result) {
        if (!guardedThis || guardedThis->currentGeneration_ != generation) return;
        if (!result.isValid() || result.isNull()) return;

        const QVariantMap map = resultMap(result);
        const QString htmlLang = map.value(QStringLiteral("htmlLang")).toString();
        const QString metaLang = map.value(QStringLiteral("metaLang")).toString();
        const QString sample = map.value(QStringLiteral("sampleText")).toString();

        const QString detected = LanguageDetector::detectLanguage(htmlLang, metaLang, sample);
        if (detected.isEmpty()) return;

        guardedThis->sourceLanguage_ = detected;
        if (LanguageDetector::isTranslatable(detected, guardedThis->targetLanguage_)) {
          if (guardedThis->service_ && guardedThis->service_->shouldNeverTranslate(detected)) {
            return;
          }
          guardedThis->state_ = State::Detected;
          emit guardedThis->languageDetected(detected, guardedThis->targetLanguage_);
          emit guardedThis->stateChanged(guardedThis->state_);
          if (!guardedThis->pendingManualTarget_.isEmpty()) {
            const QString pendingTarget = guardedThis->pendingManualTarget_;
            guardedThis->pendingManualTarget_.clear();
            guardedThis->translatePage(pendingTarget);
          } else if (guardedThis->service_ && guardedThis->service_->shouldAutoTranslate(detected)) {
            guardedThis->translatePage(guardedThis->targetLanguage_);
          }
        }
      });
}

void PageTranslator::translatePage(const QString &targetLang) {
  if (!hasPage() || !service_) return;
  if (!targetLang.isEmpty()) targetLanguage_ = targetLang;
  if (sourceLanguage_.isEmpty()) {
    pendingManualTarget_ = targetLanguage_;
    detectLanguage();
    return;
  }

  stopDynamicMutationWatcher();

  state_ = State::Translating;
  lastError_.clear();
  pendingManualTarget_.clear();
  emit stateChanged(state_);

  ensureScriptInjected();

  const uint64_t generation = ++currentGeneration_;
  QPointer<PageTranslator> guardedThis(this);

  const QString extractCall = QStringLiteral(
      "window.__daliniraTranslate ? JSON.stringify(window.__daliniraTranslate.extractNodes(%1)) : null;")
      .arg(QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("lang"), targetLanguage_}}).toJson(QJsonDocument::Compact)));

  runJavaScript(
      extractCall,
      [guardedThis, generation](const QVariant &result) {
        if (!guardedThis || guardedThis->currentGeneration_ != generation) return;
        if (!result.isValid() || result.isNull()) {
          guardedThis->state_ = State::Error;
          guardedThis->lastError_ = I18n::text(QStringLiteral("translate.extract_failed"));
          emit guardedThis->translationFinished(false, guardedThis->lastError_);
          emit guardedThis->stateChanged(guardedThis->state_);
          return;
        }

        const QVariantList nodes = resultMap(result).value(QStringLiteral("nodes")).toList();
        QList<BatchItem> items;
        items.reserve(nodes.size());

        for (const QVariant &item : nodes) {
          const QVariantMap m = item.toMap();
          items.append({
              m.value(QStringLiteral("id")).toInt(),
              m.value(QStringLiteral("text")).toString(),
              m.value(QStringLiteral("type")).toString(),
              m.value(QStringLiteral("attr")).toString()
          });
        }

        guardedThis->dispatchTranslationBatches(items, generation, false);
      });
}

void PageTranslator::dispatchTranslationBatches(const QList<BatchItem> &items, uint64_t generation, bool isDynamic) {
  if (items.isEmpty()) {
    if (!isDynamic) {
      state_ = State::Translated;
      if (hasPage()) {
        const QString startObsCall = QStringLiteral(
            "window.__daliniraTranslate ? window.__daliniraTranslate.startObserving(%1) : null;")
            .arg(QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("lang"), targetLanguage_}}).toJson(QJsonDocument::Compact)));
        runJavaScript(startObsCall);
      }
      startDynamicMutationWatcher();
      emit translationFinished(true, QString());
      emit stateChanged(state_);
    }
    return;
  }

  QList<QList<BatchItem>> batches;
  QList<BatchItem> currentBatch;

  for (const auto &item : items) {
    currentBatch.append(item);
    if (currentBatch.size() >= kMaxBatchSize) {
      batches.append(currentBatch);
      currentBatch.clear();
    }
  }
  if (!currentBatch.isEmpty()) batches.append(currentBatch);

  auto remaining = std::make_shared<int>(batches.size());
  auto hasFailed = std::make_shared<bool>(false);

  if (isDynamic) {
    isDynamicTranslating_ = true;
  }

  QPointer<PageTranslator> guardedThis(this);

  for (const auto &batch : batches) {
    QStringList texts;
    texts.reserve(batch.size());
    for (const auto &entry : batch) texts.append(entry.text);

    service_->translateBatch(
        texts, sourceLanguage_, targetLanguage_,
        [guardedThis, batch, remaining, hasFailed, generation, isDynamic](bool success, const QStringList &translated, const QString &err) {
          if (!guardedThis || guardedThis->currentGeneration_ != generation) return;

          if (!success || translated.size() != batch.size()) {
            if (!*hasFailed) {
              *hasFailed = true;
              if (!isDynamic) {
                guardedThis->state_ = State::Error;
                guardedThis->lastError_ = err.isEmpty() ? I18n::text(QStringLiteral("translate.service_error")) : err;
                emit guardedThis->translationFinished(false, guardedThis->lastError_);
                emit guardedThis->stateChanged(guardedThis->state_);
              } else {
                guardedThis->isDynamicTranslating_ = false;
              }
            }
            return;
          }

          if (*hasFailed) return;

          // Build updates JSON array
          QJsonArray updates;
          for (int i = 0; i < batch.size(); ++i) {
            QJsonObject obj;
            obj.insert(QStringLiteral("id"), batch.at(i).id);
            obj.insert(QStringLiteral("translated"), translated.at(i));
            if (!batch.at(i).type.isEmpty()) obj.insert(QStringLiteral("type"), batch.at(i).type);
            if (!batch.at(i).attr.isEmpty()) obj.insert(QStringLiteral("attr"), batch.at(i).attr);
            updates.append(obj);
          }

          const QString jsCall = QStringLiteral("window.__daliniraTranslate ? window.__daliniraTranslate.applyTranslations(%1) : null;")
                                     .arg(QString::fromUtf8(QJsonDocument(updates).toJson(QJsonDocument::Compact)));

          guardedThis->runJavaScript(jsCall);

          (*remaining)--;
          if (*remaining == 0 && !*hasFailed) {
            if (isDynamic) {
              guardedThis->isDynamicTranslating_ = false;
            } else {
              guardedThis->state_ = State::Translated;
              if (guardedThis->hasPage()) {
                const QString startObsCall = QStringLiteral(
                    "window.__daliniraTranslate ? window.__daliniraTranslate.startObserving(%1) : null;")
                    .arg(QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("lang"), guardedThis->targetLanguage_}}).toJson(QJsonDocument::Compact)));
                guardedThis->runJavaScript(startObsCall);
              }
              guardedThis->startDynamicMutationWatcher();
              emit guardedThis->translationFinished(true, QString());
              emit guardedThis->stateChanged(guardedThis->state_);
            }
          }
        });
  }
}

void PageTranslator::startDynamicMutationWatcher() {
  if (!mutationPollTimer_) {
    mutationPollTimer_ = new QTimer(this);
    connect(mutationPollTimer_, &QTimer::timeout, this, &PageTranslator::processPendingMutations);
  }
  if (!mutationPollTimer_->isActive()) {
    mutationPollTimer_->start(kMutationPollIntervalMs);
  }
}

void PageTranslator::stopDynamicMutationWatcher() {
  if (mutationPollTimer_ && mutationPollTimer_->isActive()) {
    mutationPollTimer_->stop();
  }
  isDynamicTranslating_ = false;
}

void PageTranslator::processPendingMutations() {
  if (!hasPage() || state_ != State::Translated || isDynamicTranslating_) return;

  const uint64_t generation = currentGeneration_;
  QPointer<PageTranslator> guardedThis(this);

  runJavaScript(
      QStringLiteral("window.__daliniraTranslate ? JSON.stringify(window.__daliniraTranslate.checkPendingMutations()) : null;"),
      [guardedThis, generation](const QVariant &result) {
        if (!guardedThis || guardedThis->currentGeneration_ != generation) return;
        if (!result.isValid() || result.isNull()) return;

        const QVariantList nodes = resultMap(result).value(QStringLiteral("nodes")).toList();
        if (nodes.isEmpty()) return;

        QList<BatchItem> items;
        items.reserve(nodes.size());
        for (const QVariant &item : nodes) {
          const QVariantMap m = item.toMap();
          items.append({
              m.value(QStringLiteral("id")).toInt(),
              m.value(QStringLiteral("text")).toString(),
              m.value(QStringLiteral("type")).toString(),
              m.value(QStringLiteral("attr")).toString()
          });
        }

        guardedThis->dispatchTranslationBatches(items, generation, true);
      });
}

void PageTranslator::handleSpaNavigation(const QUrl &newUrl) {
  Q_UNUSED(newUrl);
  if (state_ == State::Translated) {
    // Keep translated state active; trigger immediate mutation check
    processPendingMutations();
  } else if (state_ == State::Idle || state_ == State::Detected) {
    detectLanguage();
  }
}

void PageTranslator::restoreOriginal() {
  stopDynamicMutationWatcher();
  currentGeneration_++;
  if (hasPage()) runJavaScript(
      QStringLiteral("window.__daliniraTranslate ? window.__daliniraTranslate.restoreOriginal() : null;"));

  state_ = State::Detected;
  emit stateChanged(state_);
}

void PageTranslator::reset() {
  stopDynamicMutationWatcher();
  currentGeneration_++;
  if (hasPage()) runJavaScript(
      QStringLiteral("window.__daliniraTranslate ? window.__daliniraTranslate.reset() : null;"));
  state_ = State::Idle;
  sourceLanguage_.clear();
  targetLanguage_ = service_ && !service_->defaultTargetLanguage().isEmpty()
      ? service_->defaultTargetLanguage() : QStringLiteral("en");
  lastError_.clear();
  pendingManualTarget_.clear();
  emit stateChanged(state_);
}
