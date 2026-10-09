#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebEngineView>
#include <functional>
#include "engine/engine_view.h"

class TranslateService;

class PageTranslator : public QObject {
  Q_OBJECT

 public:
  enum class State {
    Idle,
    Detected,
    Translating,
    Translated,
    Error
  };
  Q_ENUM(State)

  PageTranslator(QWebEngineView *view, TranslateService *service, QObject *parent = nullptr);
  PageTranslator(dalinira::engine::EngineView *view, TranslateService *service,
                 QObject *parent = nullptr);
  ~PageTranslator() override;

  State state() const;
  QString sourceLanguage() const;
  QString targetLanguage() const;
  QString lastError() const;
  uint64_t currentGeneration() const;
  QWebEngineView *view() const { return view_.data(); }
  dalinira::engine::EngineView *engineView() const { return engineView_.data(); }
  TranslateService *service() const { return service_; }

  void detectLanguage();
  void translatePage(const QString &targetLang = QStringLiteral("tr"));
  void restoreOriginal();
  void reset();
  void handleSpaNavigation(const QUrl &newUrl);

 signals:
  void stateChanged(PageTranslator::State newState);
  void languageDetected(const QString &sourceLang, const QString &targetLang);
  void translationFinished(bool success, const QString &error);

 private:
  void ensureScriptInjected();
  void startDynamicMutationWatcher();
  void stopDynamicMutationWatcher();
  void processPendingMutations();

  struct BatchItem {
    int id;
    QString text;
    QString type;
    QString attr;
  };

  void dispatchTranslationBatches(const QList<BatchItem> &items, uint64_t generation, bool isDynamic);
  bool hasPage() const;
  QUrl pageUrl() const;
  void runJavaScript(const QString &script,
                     std::function<void(const QVariant &)> callback = {});

  QPointer<QWebEngineView> view_;
  QPointer<dalinira::engine::EngineView> engineView_;
  TranslateService *service_ = nullptr;
  State state_ = State::Idle;
  QString sourceLanguage_;
  QString targetLanguage_ = QStringLiteral("tr");
  QString lastError_;

  uint64_t currentGeneration_ = 1;
  QTimer *mutationPollTimer_ = nullptr;
  bool isDynamicTranslating_ = false;
  QString pendingManualTarget_;
};
