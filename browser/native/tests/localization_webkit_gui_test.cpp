// WebKit localization GUI validation: the shared New Tab document served by
// the WebKit scheme handler shows the same catalog strings as Chromium in
// every UI language, declares lang + dir="ltr", and keeps engine-specific
// Frequent Sites data (only the UI language is shared).
//
// Runs through scripts/safe-gui-test.sh: one window, one WebKit view for the
// whole run, no core dumps (webkit_gui_harness.h).

#include "webkit_gui_harness.h"

#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/webkit/webkit_backend.h"
#include "i18n/language_manager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

using dalinira::engine::EngineBackend;
using dalinira::engine::EngineType;
using dalinira::engine::WebKitBackend;
using dalinira::engine::WebKitEngineView;
using dalinira::i18n::LanguageManager;
using dalinira::test::WebKitGuiHarness;

int main(int argc, char *argv[]) {
  WebKitGuiHarness::disableCoreDumps();
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("GDK_BACKEND", "x11");
  qunsetenv("DALINIRA_UI_LANGUAGE");
  QApplication app(argc, argv);
  QTemporaryDir settingsRoot;
  WK_CHECK(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("LocalizationWebKitGuiTest"));
  std::cout << "=== WEBKIT LOCALIZATION GUI VALIDATION ===" << std::endl;

  auto &lm = LanguageManager::instance();
  lm.initialize();

  QTemporaryDir storageRoot;
  WK_CHECK(storageRoot.isValid());
  dalinira::engine::EngineStorageManager storage(storageRoot.path());
  storage.ensureDirectories(EngineType::WebKit);
  auto backend = std::make_unique<WebKitBackend>(nullptr);
  WK_CHECK(backend->initialize(storage));
  EngineBackend::registerBackend(EngineType::WebKit, std::move(backend));

  WebKitGuiHarness h(app);
  auto *view = h.attach(qobject_cast<WebKitEngineView *>(
                            EngineBackend::backendForType(EngineType::WebKit)->createView(h.stack())),
                        "localized new tab");
  WK_CHECK(view != nullptr);

  for (const QString &language : {QStringLiteral("tr"), QStringLiteral("en"), QStringLiteral("ar"),
                                  QStringLiteral("fr"), QStringLiteral("tr")}) {
    lm.setLanguagePreference(language);
    WK_CHECK(QGuiApplication::layoutDirection() == Qt::LeftToRight);
    WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 8000));
    const QString json = h.js(view, QStringLiteral(
        "JSON.stringify({lang:document.documentElement.lang,dir:document.documentElement.dir,"
        "bodyDir:getComputedStyle(document.body).direction,"
        "placeholder:document.querySelector('#query')?.placeholder||'',"
        "customize:document.querySelector('#customization-title')?.textContent||'',"
        "restore:document.querySelector('#restore-frequent-sites')?.textContent||'',"
        "frequent:document.querySelector('#shortcuts')?.getAttribute('aria-label')||'',"
        "tokens:document.documentElement.outerHTML.includes('{{i18n')})")).toString();
    const QJsonObject page = QJsonDocument::fromJson(json.toUtf8()).object();
    std::cout << "  [" << language.toStdString() << "] " << json.toStdString() << std::endl;
    WK_CHECK(page.value(QStringLiteral("lang")).toString() == language);
    WK_CHECK(page.value(QStringLiteral("dir")).toString() == QLatin1String("ltr"));
    WK_CHECK(page.value(QStringLiteral("bodyDir")).toString() == QLatin1String("ltr"));
    WK_CHECK(!page.value(QStringLiteral("tokens")).toBool());
    // Same fallback as BrowserWindow::currentSearchEngine() without a profile.
    const QString engine = QSettings().value(QStringLiteral("browser/searchEngine"), QStringLiteral("DuckDuckGo")).toString();
    WK_CHECK(page.value(QStringLiteral("placeholder")).toString()
             == lm.translateIn(language, QStringLiteral("newtab.search_placeholder_") + engine.toLower()));
    WK_CHECK(page.value(QStringLiteral("customize")).toString()
             == lm.translateIn(language, QStringLiteral("newtab.customize_title")));
    WK_CHECK(page.value(QStringLiteral("restore")).toString()
             == lm.translateIn(language, QStringLiteral("common.restore")));
    WK_CHECK(page.value(QStringLiteral("frequent")).toString()
             == lm.translateIn(language, QStringLiteral("newtab.frequent_sites")));
    view->grab().save(QDir::current().filePath(QStringLiteral("gui-test-logs/localization-webkit-newtab-%1.png").arg(language)));
  }
  std::cout << "[PASS] WebKit New Tab is localized through tr/en/ar/fr/tr with LTR layout." << std::endl;

  h.destroyView(view);
  EngineBackend::shutdown();
  h.waitForAllWebKitProcessesToExit();
  return 0;
}
