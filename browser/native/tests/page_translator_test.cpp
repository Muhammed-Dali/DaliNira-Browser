#include <QApplication>
#include <QPointer>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebEngineScript>
#include <cassert>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <tuple>

#include "page_translator.h"
#include "language_detector.h"
#include "translate_service.h"
#include "translate_engine_script.h"

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  QApplication app(argc, argv);

  // Test 1: PageTranslator is parented to view by default
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    assert(translator->parent() == view);
    assert(translator->state() == PageTranslator::State::Idle);
    assert(translator->targetLanguage() == QStringLiteral("tr"));
    delete view; // View deletion must clean up child translator without leak or crash
  }

  // Test 2: Destruction of view cleans up PageTranslator via QPointer (no memory leak)
  {
    QPointer<PageTranslator> guardedTranslator;
    {
      auto *view = new QWebEngineView();
      auto *translator = new PageTranslator(view, nullptr);
      guardedTranslator = translator;
      assert(!guardedTranslator.isNull());
      delete view;
    }
    assert(guardedTranslator.isNull());
  }

  // Test 3: findChild pattern reuses existing translator on view (no duplicates across detach/attach)
  {
    auto *view = new QWebEngineView();
    auto *translator1 = view->findChild<PageTranslator *>(QString(), Qt::FindDirectChildrenOnly);
    assert(translator1 == nullptr);

    translator1 = new PageTranslator(view, nullptr, view);
    auto *translator2 = view->findChild<PageTranslator *>(QString(), Qt::FindDirectChildrenOnly);
    assert(translator2 != nullptr);
    assert(translator1 == translator2);

    delete view;
  }

  // Test 4: Reset cleans up state, increments generation, and stops dynamic watcher
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    const uint64_t genBefore = translator->currentGeneration();
    translator->reset();
    assert(translator->currentGeneration() > genBefore);
    assert(translator->state() == PageTranslator::State::Idle);
    assert(translator->sourceLanguage().isEmpty());
    assert(translator->lastError().isEmpty());
    delete view;
  }

  // Test 5: restoreOriginal transitions state and stops watcher
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    translator->restoreOriginal();
    assert(translator->state() == PageTranslator::State::Detected);
    delete view;
  }

  // Test 6: Non-web / invalid URL schemes reset safely without crashes
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    view->setUrl(QUrl(QStringLiteral("about:blank")));
    translator->detectLanguage();
    assert(translator->state() == PageTranslator::State::Idle);
    delete view;
  }

  // Test 7: Signal emission verifies state changes
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    int stateChangeCount = 0;
    PageTranslator::State recordedState = PageTranslator::State::Idle;
    QObject::connect(translator, &PageTranslator::stateChanged, [&](PageTranslator::State newState) {
      ++stateChangeCount;
      recordedState = newState;
    });
    translator->restoreOriginal();
    assert(stateChangeCount == 1);
    assert(recordedState == PageTranslator::State::Detected);
    delete view;
  }

  // Test 8: service() accessor returns provided TranslateService
  {
    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, nullptr);
    assert(translator->service() == nullptr);
    delete view;
  }

  // Test 9: LanguageDetector translatability logic (same-language, foreign-language, invalid codes)
  {
    assert(LanguageDetector::isTranslatable(QStringLiteral("en"), QStringLiteral("tr")));
    assert(LanguageDetector::isTranslatable(QStringLiteral("fr"), QStringLiteral("tr")));
    assert(LanguageDetector::isTranslatable(QStringLiteral("de"), QStringLiteral("tr")));
    assert(!LanguageDetector::isTranslatable(QStringLiteral("tr"), QStringLiteral("tr"))); // Same language rejected
    assert(!LanguageDetector::isTranslatable(QString(), QStringLiteral("tr"))); // Empty / unreliable rejected
    assert(!LanguageDetector::isTranslatable(QStringLiteral("und"), QStringLiteral("tr"))); // Undefined rejected
    assert(LanguageDetector::languageDisplayName(QStringLiteral("en")) == QStringLiteral("İngilizce"));
    assert(LanguageDetector::languageDisplayName(QStringLiteral("tr")) == QStringLiteral("Türkçe"));
  }

  // Test 10: Arabic translatability and display name
  {
    assert(LanguageDetector::isTranslatable(QStringLiteral("en"), QStringLiteral("ar")));
    assert(LanguageDetector::isTranslatable(QStringLiteral("tr"), QStringLiteral("ar")));
    assert(!LanguageDetector::isTranslatable(QStringLiteral("ar"), QStringLiteral("ar")));
    assert(LanguageDetector::languageDisplayName(QStringLiteral("ar")) == QStringLiteral("Arapça"));
  }

  // Test 11: PageTranslator default target language synchronization with TranslateService
  {
    TranslateService service;
    service.setDefaultTargetLanguage(QStringLiteral("ar"));
    assert(service.defaultTargetLanguage() == QStringLiteral("ar"));

    auto *view = new QWebEngineView();
    auto *translator = new PageTranslator(view, &service);
    assert(translator->targetLanguage() == QStringLiteral("ar"));

    translator->reset();
    assert(translator->targetLanguage() == QStringLiteral("ar"));

    delete view;
  }

  // Test 12: Regression test: English original -> Turkish -> Arabic -> restore original
  // Must retranslate from ORIGINAL English text when changing target from Turkish -> Arabic (not from Turkish output),
  // must not skip previously translated nodes, and must restore cleanly without accumulating artifacts.
  {
    auto *view = new QWebEngineView();
    QEventLoop loop;
    QObject::connect(view, &QWebEngineView::loadFinished, &loop, &QEventLoop::quit);

    const QString html = QStringLiteral(
        "<!DOCTYPE html>"
        "<html><head><title>Test Page</title></head><body>"
        "<h1 id='hero'>The future of building together</h1>"
        "<p id='sub'>Accelerate your entire workflow with tools and trends.</p>"
        "<nav id='nav'>"
        "  <a id='nav1' title='Platform tools'>Platform</a>"
        "  <a id='nav2'>Enterprise</a>"
        "</nav>"
        "</body></html>");

    view->setHtml(html);
    loop.exec();

    // Helper lambda to run JS synchronously on view's page
    auto runJs = [&](const QString &js) -> QVariant {
      QEventLoop jsLoop;
      QVariant res;
      view->page()->runJavaScript(js, QWebEngineScript::ApplicationWorld, [&](const QVariant &v) {
        res = v;
        jsLoop.quit();
      });
      jsLoop.exec();
      return res;
    };

    // 1. Inject translate engine script
    runJs(translateEngineScript());

    // 2. Extract nodes for Turkish translation (target = 'tr')
    QVariant extTr = runJs(QStringLiteral("window.__daliniraTranslate.extractNodes({ lang: 'tr' });"));
    QVariantList trNodes = extTr.toMap().value(QStringLiteral("nodes")).toList();
    assert(!trNodes.isEmpty());

    // Verify all original English texts were extracted
    bool foundHero = false;
    bool foundSub = false;
    bool foundNav1 = false;
    bool foundNav2 = false;
    bool foundNav1Attr = false;

    int heroId = -1;
    int subId = -1;
    int nav1Id = -1;
    int nav2Id = -1;
    int nav1AttrId = -1;

    for (const QVariant &v : trNodes) {
      QVariantMap m = v.toMap();
      const QString text = m.value(QStringLiteral("text")).toString();
      const QString type = m.value(QStringLiteral("type")).toString();
      const int id = m.value(QStringLiteral("id")).toInt();
      if (type == QStringLiteral("text")) {
        if (text == QStringLiteral("The future of building together")) {
          foundHero = true;
          heroId = id;
        } else if (text == QStringLiteral("Accelerate your entire workflow with tools and trends.")) {
          foundSub = true;
          subId = id;
        } else if (text == QStringLiteral("Platform")) {
          foundNav1 = true;
          nav1Id = id;
        } else if (text == QStringLiteral("Enterprise")) {
          foundNav2 = true;
          nav2Id = id;
        }
      } else if (type == QStringLiteral("attr")) {
        if (text == QStringLiteral("Platform tools")) {
          foundNav1Attr = true;
          nav1AttrId = id;
        }
      }
    }

    assert(foundHero && foundSub && foundNav1 && foundNav2 && foundNav1Attr);

    // 3. Apply Turkish translations
    QJsonArray trUpdates;
    trUpdates.append(QJsonObject{{QStringLiteral("id"), heroId}, {QStringLiteral("translated"), QStringLiteral("Binanın geleceği birlikte olur")}});
    trUpdates.append(QJsonObject{{QStringLiteral("id"), subId}, {QStringLiteral("translated"), QStringLiteral("Tüm iş akışınızı hızlandırın")}});
    trUpdates.append(QJsonObject{{QStringLiteral("id"), nav1Id}, {QStringLiteral("translated"), QStringLiteral("Platform")}});
    trUpdates.append(QJsonObject{{QStringLiteral("id"), nav2Id}, {QStringLiteral("translated"), QStringLiteral("Kurumsal")}});
    trUpdates.append(QJsonObject{{QStringLiteral("id"), nav1AttrId}, {QStringLiteral("type"), QStringLiteral("attr")}, {QStringLiteral("attr"), QStringLiteral("title")}, {QStringLiteral("translated"), QStringLiteral("Platform araçları")}});

    runJs(QStringLiteral("window.__daliniraTranslate.applyTranslations(%1);")
          .arg(QString::fromUtf8(QJsonDocument(trUpdates).toJson(QJsonDocument::Compact))));

    // Verify DOM now displays Turkish
    assert(runJs(QStringLiteral("document.getElementById('hero').innerText;")).toString() == QStringLiteral("Binanın geleceği birlikte olur"));
    assert(runJs(QStringLiteral("document.getElementById('sub').innerText;")).toString() == QStringLiteral("Tüm iş akışınızı hızlandırın"));
    assert(runJs(QStringLiteral("document.getElementById('nav2').innerText;")).toString() == QStringLiteral("Kurumsal"));
    assert(runJs(QStringLiteral("document.getElementById('nav1').getAttribute('title');")).toString() == QStringLiteral("Platform araçları"));

    // 4. Without restoring the page, change target from Turkish -> Arabic (target = 'ar')
    QVariant extAr = runJs(QStringLiteral("window.__daliniraTranslate.extractNodes({ lang: 'ar' });"));
    QVariantList arNodes = extAr.toMap().value(QStringLiteral("nodes")).toList();
    assert(!arNodes.isEmpty());

    // CRITICAL ASSERTION: All extracted nodes MUST have original English text, NEVER Turkish!
    for (const QVariant &v : arNodes) {
      QVariantMap m = v.toMap();
      const QString text = m.value(QStringLiteral("text")).toString();
      // None of the extracted texts should be the Turkish translation output!
      assert(text != QStringLiteral("Binanın geleceği birlikte olur"));
      assert(text != QStringLiteral("Tüm iş akışınızı hızlandırın"));
      assert(text != QStringLiteral("Kurumsal"));
      assert(text != QStringLiteral("Platform araçları"));
    }

    // Verify original English strings were extracted again
    foundHero = foundSub = foundNav1 = foundNav2 = foundNav1Attr = false;
    for (const QVariant &v : arNodes) {
      QVariantMap m = v.toMap();
      const QString text = m.value(QStringLiteral("text")).toString();
      const QString type = m.value(QStringLiteral("type")).toString();
      if (type == QStringLiteral("text")) {
        if (text == QStringLiteral("The future of building together")) foundHero = true;
        else if (text == QStringLiteral("Accelerate your entire workflow with tools and trends.")) foundSub = true;
        else if (text == QStringLiteral("Platform")) foundNav1 = true;
        else if (text == QStringLiteral("Enterprise")) foundNav2 = true;
      } else if (type == QStringLiteral("attr")) {
        if (text == QStringLiteral("Platform tools")) foundNav1Attr = true;
      }
    }
    assert(foundHero && foundSub && foundNav1 && foundNav2 && foundNav1Attr);

    // 5. Apply Arabic translations
    QJsonArray arUpdates;
    arUpdates.append(QJsonObject{{QStringLiteral("id"), heroId}, {QStringLiteral("translated"), QString::fromUtf8("مستقبل البناء معاً")}});
    arUpdates.append(QJsonObject{{QStringLiteral("id"), subId}, {QStringLiteral("translated"), QString::fromUtf8("تسريع سير عملك بالكامل")}});
    arUpdates.append(QJsonObject{{QStringLiteral("id"), nav1Id}, {QStringLiteral("translated"), QString::fromUtf8("منصة")}});
    arUpdates.append(QJsonObject{{QStringLiteral("id"), nav2Id}, {QStringLiteral("translated"), QString::fromUtf8("مؤسسات")}});
    arUpdates.append(QJsonObject{{QStringLiteral("id"), nav1AttrId}, {QStringLiteral("type"), QStringLiteral("attr")}, {QStringLiteral("attr"), QStringLiteral("title")}, {QStringLiteral("translated"), QString::fromUtf8("أدوات المنصة")}});

    runJs(QStringLiteral("window.__daliniraTranslate.applyTranslations(%1);")
          .arg(QString::fromUtf8(QJsonDocument(arUpdates).toJson(QJsonDocument::Compact))));

    // Verify DOM now displays Arabic
    assert(runJs(QStringLiteral("document.getElementById('hero').innerText;")).toString() == QString::fromUtf8("مستقبل البناء معاً"));
    assert(runJs(QStringLiteral("document.getElementById('sub').innerText;")).toString() == QString::fromUtf8("تسريع سير عملك بالكامل"));
    assert(runJs(QStringLiteral("document.getElementById('nav2').innerText;")).toString() == QString::fromUtf8("مؤسسات"));
    assert(runJs(QStringLiteral("document.getElementById('nav1').getAttribute('title');")).toString() == QString::fromUtf8("أدوات المنصة"));

    // 6. Restore original
    runJs(QStringLiteral("window.__daliniraTranslate.restoreOriginal();"));

    // Verify DOM is cleanly restored to original English without any Turkish or Arabic remnants
    assert(runJs(QStringLiteral("document.getElementById('hero').innerText;")).toString() == QStringLiteral("The future of building together"));
    assert(runJs(QStringLiteral("document.getElementById('sub').innerText;")).toString() == QStringLiteral("Accelerate your entire workflow with tools and trends."));
    assert(runJs(QStringLiteral("document.getElementById('nav1').innerText;")).toString() == QStringLiteral("Platform"));
    assert(runJs(QStringLiteral("document.getElementById('nav2').innerText;")).toString() == QStringLiteral("Enterprise"));
    assert(runJs(QStringLiteral("document.getElementById('nav1').getAttribute('title');")).toString() == QStringLiteral("Platform tools"));

    delete view;
  }

  // Test 13: deterministic Chromium automatic-offer matrix. setHtml receives
  // an HTTP base URL so PageTranslator follows the same eligibility path as a
  // network page without contacting a live site.
  {
    const QString english = QStringLiteral(
        "<!doctype html><html%1><body><p>The browser page contains enough English text "
        "for reliable automatic language detection and a deterministic translation offer.</p></body></html>");
    auto detect = [&](const QString &html, const QString &target) {
      auto *view = new QWebEngineView();
      QEventLoop loadLoop;
      QObject::connect(view, &QWebEngineView::loadFinished, &loadLoop, &QEventLoop::quit);
      view->setHtml(html, QUrl(QStringLiteral("https://local.dalinira.test/language")));
      loadLoop.exec();
      TranslateService service;
      service.setDefaultTargetLanguage(target);
      auto *translator = new PageTranslator(view, &service);
      translator->detectLanguage();
      QEventLoop detectLoop;
      QTimer poll;
      poll.setInterval(10);
      QObject::connect(&poll, &QTimer::timeout, &detectLoop, [&] {
        if (!translator->sourceLanguage().isEmpty()) detectLoop.quit();
      });
      QTimer::singleShot(3000, &detectLoop, &QEventLoop::quit);
      poll.start();
      detectLoop.exec();
      const QString source = translator->sourceLanguage();
      const PageTranslator::State state = translator->state();
      const QString actualTarget = translator->targetLanguage();
      delete view;
      return std::tuple{source, state, actualTarget};
    };

    const auto [trSource, trState, trTarget] = detect(english.arg(QStringLiteral(" lang='en'")), QStringLiteral("tr"));
    assert(trSource == QLatin1String("en") && trTarget == QLatin1String("tr") && trState == PageTranslator::State::Detected);
    const auto [enSource, enState, enTarget] = detect(english.arg(QStringLiteral(" lang='en'")), QStringLiteral("en"));
    assert(enSource == QLatin1String("en") && enTarget == QLatin1String("en") && enState == PageTranslator::State::Idle);
    const auto [arSource, arState, arTarget] = detect(english.arg(QStringLiteral(" lang='en'")), QStringLiteral("ar"));
    assert(arSource == QLatin1String("en") && arTarget == QLatin1String("ar") && arState == PageTranslator::State::Detected);
    const auto [frSource, frState, frTarget] = detect(english.arg(QStringLiteral(" lang='en'")), QStringLiteral("fr"));
    assert(frSource == QLatin1String("en") && frTarget == QLatin1String("fr") && frState == PageTranslator::State::Detected);
    const auto [fallbackSource, fallbackState, fallbackTarget] = detect(english.arg(QString()), QStringLiteral("tr"));
    assert(fallbackSource == QLatin1String("en") && fallbackTarget == QLatin1String("tr") && fallbackState == PageTranslator::State::Detected);
  }

  return 0;
}
