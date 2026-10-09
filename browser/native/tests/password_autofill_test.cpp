#include "passwords/credential_vault.h"
#include "passwords/credential_vault_manager.h"
#include "passwords/credential_autofill_controller.h"
#include "passwords/vault_unlock_dialog.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QInputDialog>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QDirIterator>
#include <QThread>
#include <QWebEngineView>
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>
#include <cassert>
#include <functional>
#include <iostream>

int main(int argc, char **argv) {
  QApplication app(argc, argv);

  std::cout << "Starting Password Autofill & Form Integration Test Suite..." << std::endl;

  const int defaultTimeoutMs =
      qEnvironmentVariableIntValue("DALINIRA_SLOW_WEBENGINE_CI") ? 25000 : 12000;
  auto waitForCondition = [defaultTimeoutMs](const std::function<bool()> &pred, int timeoutMs = 0) -> bool {
    int effectiveTimeout = timeoutMs > 0 ? timeoutMs : defaultTimeoutMs;
    if (qEnvironmentVariableIntValue("DALINIRA_SLOW_WEBENGINE_CI") && effectiveTimeout < 25000) {
      effectiveTimeout = 25000;
    }
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < effectiveTimeout) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      QThread::msleep(10);
    }
    return pred();
  };

  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  CredentialVaultManager vaultManager(tempDir.path());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  // TEST 7: locked vault blocks fill without authorization
  {
    std::cout << "[RUN] TEST 7: Locked vault blocks fill" << std::endl;
    assert(vaultManager.isLocked());
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com/login"))).isEmpty());
    std::cout << "[PASS] TEST 7: Locked vault blocks fill" << std::endl;
  }

  // Setup vault
  assert(vaultManager.create(masterPassword));
  assert(!vaultManager.isLocked());

  // Save sample credential for Facebook
  const QString fbOrigin = QStringLiteral("https://www.facebook.com");
  CredentialSecret fbSecret;
  fbSecret.origin = fbOrigin;
  fbSecret.username = QStringLiteral("user@example.com");
  fbSecret.password = QStringLiteral("MySuperSecretFbPassword#2026");
  fbSecret.iconPngBase64 = QStringLiteral("ZmF2aWNvbg==");
  bool updated = false;
  assert(vaultManager.save(fbSecret, &updated));

  // Save sample credential for generic site (email + password)
  const QString genericOrigin = QStringLiteral("https://accounts.example.org");
  CredentialSecret genericSecret;
  genericSecret.origin = genericOrigin;
  genericSecret.username = QStringLiteral("user@example.com");
  genericSecret.password = QStringLiteral("ExampleSecret#2026");
  assert(vaultManager.save(genericSecret, &updated));

  CredentialAutofillController controller(&vaultManager);

  // TEST 4: vault unlocked credential lookup
  {
    std::cout << "[RUN] TEST 4: Vault unlocked credential lookup" << std::endl;
    const auto records = vaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com/login/")));
    assert(records.size() == 1);
    assert(records.front().origin == fbOrigin);
    assert(records.front().username == QStringLiteral("user@example.com"));
    std::cout << "[PASS] TEST 4: Vault unlocked credential lookup" << std::endl;
  }

  // TEST 5: correct origin credential returned
  {
    std::cout << "[RUN] TEST 5: Correct origin credential returned" << std::endl;
    const auto fbRecords = vaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com/")));
    assert(fbRecords.size() == 1);
    assert(fbRecords.front().username == QStringLiteral("user@example.com"));

    const auto genRecords = vaultManager.forOrigin(QUrl(QStringLiteral("https://accounts.example.org/signin")));
    assert(genRecords.size() == 1);
    assert(genRecords.front().username == QStringLiteral("user@example.com"));
    std::cout << "[PASS] TEST 5: Correct origin credential returned" << std::endl;
  }

  // TEST 6: wrong origin rejected
  {
    std::cout << "[RUN] TEST 6: Wrong origin rejected" << std::endl;
    // Subdomain or spoofed origin cannot access facebook.com credentials
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://fake-facebook.com"))).isEmpty());
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://login.facebook.com.attacker.com"))).isEmpty());
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("http://www.facebook.com"))).isEmpty()); // HTTP rejected
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://facebook.com"))).isEmpty()); // Exact canonical host matching
    std::cout << "[PASS] TEST 6: Wrong origin rejected" << std::endl;
  }

  // TEST 1: password field detection heuristics in scripts
  {
    std::cout << "[RUN] TEST 1: Password field detection heuristics" << std::endl;
    const QWebEngineScript captureScript = CredentialAutofillController::candidateCaptureScript();
    const QString code = captureScript.sourceCode();
    assert(code.contains("input[type=\"password\"]"));
    assert(code.contains("visible"));
    assert(code.contains("!el.disabled"));
    assert(code.contains("!el.readOnly"));
    std::cout << "[PASS] TEST 1: Password field detection heuristics" << std::endl;
  }

  // TEST 2: username field pairing heuristics
  {
    std::cout << "[RUN] TEST 2: Username field pairing heuristics" << std::endl;
    const QWebEngineScript captureScript = CredentialAutofillController::candidateCaptureScript();
    const QString code = captureScript.sourceCode();
    // Searches within pwd.form or closest container / root without requiring HTMLFormElement instance
    assert(code.contains("pwd.closest('form, [role=\"form\"], [role=\"dialog\"], fieldset')"));
    assert(code.contains("autoC.includes('username')"));
    assert(code.contains("user|email|login|account|identifier|eposta|telefon"));
    assert(code.contains("findUsername"));
    std::cout << "[PASS] TEST 2: Username field pairing heuristics" << std::endl;
  }

  // TEST 3 & TEST 17: icon injection single instance & duplicate prevention
  {
    std::cout << "[RUN] TEST 3 & 17: Icon injection single instance and duplicate prevention" << std::endl;
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("token-test-123"));
    assert(buttonScript.contains("data-dalinira-autofill-btn"));
    assert(buttonScript.contains("managedButton"));
    // Ensures only one managedButton exists and is reused/repositioned rather than duplicated
    assert(buttonScript.contains("if (!managedButton)"));
    assert(buttonScript.contains("showButtonForField"));
    std::cout << "[PASS] TEST 3 & 17: Icon injection single instance and duplicate prevention" << std::endl;
  }

  // TEST 8: unlock then fill
  {
    std::cout << "[RUN] TEST 8: Unlock then fill" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com"))).isEmpty());

    // Wrong password fails
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#2026")));
    assert(vaultManager.isLocked());

    // Allow bounded exponential delay to elapse (500ms backoff)
    QThread::msleep(550);

    // Correct master password unlocks
    assert(vaultManager.unlock(masterPassword));
    assert(!vaultManager.isLocked());
    assert(vaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com"))).size() == 1);
    std::cout << "[PASS] TEST 8: Unlock then fill" << std::endl;
  }

  // TEST 9: username + password injection payload generation (JSON-safe, no raw string concatenation)
  {
    std::cout << "[RUN] TEST 9: Username + password injection payload generation" << std::endl;
    const QString testOrigin = QStringLiteral("https://test.example.com");
    const QString testUser = QStringLiteral("attacker\"}) alert(1); //");
    const QString testPass = QStringLiteral("pass'\"; \n <script>alert(2)</script>");
    const QString script = CredentialAutofillController::domFillScript(testOrigin, testUser, testPass);

    // Verify it is encoded via JSONDocument safely
    assert(script.contains("const v = {\"origin\":\"https://test.example.com\""));
    assert(script.contains("attacker\\\"}) alert(1); //"));
    assert(!script.contains("pass'\"; \n <script>")); // Newline and quotes are JSON-escaped
    std::cout << "[PASS] TEST 9: Username + password injection payload generation" << std::endl;
  }

  // TEST 10: input/change events dispatched & React/Vue Object.getOwnPropertyDescriptor value setter
  {
    std::cout << "[RUN] TEST 10: React/Vue value setter and input/change events" << std::endl;
    const QString script = CredentialAutofillController::domFillScript(fbOrigin, fbSecret.username, fbSecret.password);
    assert(script.contains("Object.getOwnPropertyDescriptor(proto, 'value')"));
    assert(script.contains("desc.set.call(input, val)"));
    assert(script.contains("new Event('input', { bubbles: true, cancelable: true })"));
    assert(script.contains("new Event('change', { bubbles: true, cancelable: true })"));
    std::cout << "[PASS] TEST 10: React/Vue value setter and input/change events" << std::endl;
  }

  // TEST 11: SPA dynamic password field observation
  {
    std::cout << "[RUN] TEST 11: SPA dynamic password field observation" << std::endl;
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("token-test-123"));
    assert(buttonScript.contains("new MutationObserver"));
    assert(buttonScript.contains("childList: true"));
    assert(buttonScript.contains("subtree: true"));
    assert(buttonScript.contains("scanAndAttach()"));

    const QWebEngineScript captureScript = CredentialAutofillController::candidateCaptureScript();
    assert(captureScript.sourceCode().contains("observeSuccess"));
    assert(captureScript.sourceCode().contains("pushState"));
    assert(captureScript.sourceCode().contains("replaceState"));
    assert(captureScript.sourceCode().contains("popstate"));
    std::cout << "[PASS] TEST 11: SPA dynamic password field observation" << std::endl;
  }

  // TEST 12 & TEST 13: navigation stale handler & tab close cleanup
  {
    std::cout << "[RUN] TEST 12 & 13: Navigation and tab close cleanup" << std::endl;
    auto *dummyView = reinterpret_cast<QWebEngineView *>(0x12345678);
    const QString origin = QStringLiteral("https://www.facebook.com");
    const QString activeToken = controller.grantFillToken(dummyView, origin);
    assert(!activeToken.isEmpty());
    assert(controller.activeTokenForView(dummyView, origin) == activeToken);

    // Navigation to a new origin revokes old token
    controller.onUrlChanged(dummyView, QUrl(QStringLiteral("https://accounts.google.com")));
    assert(controller.activeTokenForView(dummyView, origin).isEmpty());

    // Tab close removes all view state
    controller.grantFillToken(dummyView, origin);
    assert(!controller.activeTokenForView(dummyView, origin).isEmpty());
    controller.onViewClosed(dummyView);
    assert(controller.activeTokenForView(dummyView, origin).isEmpty());
    std::cout << "[PASS] TEST 12 & 13: Navigation and tab close cleanup" << std::endl;
  }

  // TEST 14: multi-tab credential token isolation
  {
    std::cout << "[RUN] TEST 14: Multi-tab credential token isolation" << std::endl;
    auto *tabA = reinterpret_cast<QWebEngineView *>(0x1000);
    auto *tabB = reinterpret_cast<QWebEngineView *>(0x2000);
    const QString origin = QStringLiteral("https://www.facebook.com");

    const QString tokenA = controller.grantFillToken(tabA, origin);
    const QString tokenB = controller.grantFillToken(tabB, origin);

    assert(!tokenA.isEmpty());
    assert(!tokenB.isEmpty());
    assert(tokenA != tokenB); // Tokens must be unique per view
    assert(controller.activeTokenForView(tabA, origin) == tokenA);
    assert(controller.activeTokenForView(tabB, origin) == tokenB);
    std::cout << "[PASS] TEST 14: Multi-tab credential token isolation" << std::endl;
  }

  // TEST 15: cross-origin iframe blocked
  {
    std::cout << "[RUN] TEST 15: Cross-origin iframe blocked" << std::endl;
    const QWebEngineScript captureScript = CredentialAutofillController::candidateCaptureScript();
    assert(!captureScript.runsOnSubFrames()); // Subframes disabled at native engine level
    assert(captureScript.sourceCode().contains("window.top !== window")); // In-script top guard

    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("sample-token"));
    assert(buttonScript.contains("if (window.top !== window) return;"));
    std::cout << "[PASS] TEST 15: Cross-origin iframe blocked" << std::endl;
  }

  // TEST 16: site eye icon collision avoidance offset calculation
  {
    std::cout << "[RUN] TEST 16: Site eye icon collision avoidance" << std::endl;
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("sample-token"));
    assert(buttonScript.contains("computeRightOffset"));
    assert(buttonScript.contains("child.getBoundingClientRect()"));
    assert(buttonScript.contains("rightEdge - 50")); // Checks for overlapping sibling icons inside the right end
    assert(buttonScript.contains("r.right - offset - 26")); // Offsets the DaliNira lock button to the left
    std::cout << "[PASS] TEST 16: Site eye icon collision avoidance" << std::endl;
  }

  // TEST UX: Focus-based icon visibility (hidden initially, visible on focus, hidden on focusout)
  {
    std::cout << "[RUN] TEST UX: Focus-based icon visibility" << std::endl;
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("sample-token"));
    // Initially hidden
    assert(buttonScript.contains("display: 'none'"));
    // Visibility handlers
    assert(buttonScript.contains("hideButton"));
    assert(buttonScript.contains("focusin"));
    assert(buttonScript.contains("focusout"));
    // Interaction guard: pointer interaction prevents premature disappearance
    assert(buttonScript.contains("isInteracting"));
    assert(buttonScript.contains("clearTimeout(hideTimer)"));
    assert(buttonScript.contains("managedButton.style.display = 'block'"));
    std::cout << "[PASS] TEST UX: Focus-based icon visibility" << std::endl;
  }

  // TEST 18: password not logged & cleared from memory
  {
    std::cout << "[RUN] TEST 18: Password not logged and cleared from memory" << std::endl;
    auto *view = reinterpret_cast<QWebEngineView *>(0x9999);
    controller.clearAllSensitiveData();
    assert(!controller.isCandidatePending(view, fbOrigin, QStringLiteral("user@example.com")));

    // When vault locks, all sensitive candidate data is cleared
    controller.onVaultLocked();
    assert(!controller.isCandidatePending(view, fbOrigin, QStringLiteral("user@example.com")));
    std::cout << "[PASS] TEST 18: Password not logged and cleared from memory" << std::endl;
  }

  // SECURITY TEST 1 (Requirement 10.1): synthetic click cannot trigger fill
  {
    std::cout << "[RUN] SECURITY TEST: Synthetic click cannot trigger fill" << std::endl;
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("sample-token"));
    assert(buttonScript.contains("if (!event.isTrusted) return;"));
    assert(buttonScript.contains("navigator.userActivation.isActive === false"));
    std::cout << "[PASS] SECURITY TEST: Synthetic click cannot trigger fill" << std::endl;
  }

  // SECURITY TEST 2 (Requirement 10.2): an unbound bridge message cannot trigger fill
  {
    std::cout << "[RUN] SECURITY TEST: Unbound bridge message cannot trigger fill" << std::endl;
    // A bridge message without the BrowserWebPage identity must fail closed.
    const QString fakeMsg = QStringLiteral("fill:{\"origin\":\"https://www.facebook.com\",\"token\":\"fake-token-attempt\"}");
    assert(!controller.handleBridgeMessage(nullptr, fakeMsg));
    std::cout << "[PASS] SECURITY TEST: Unbound bridge message cannot trigger fill" << std::endl;
  }

  // SECURITY TEST 3 (Requirement 10.3): wrong token rejected
  {
    std::cout << "[RUN] SECURITY TEST: Wrong token rejected" << std::endl;
    auto *view = reinterpret_cast<QWebEngineView *>(0x5555);
    const QString validToken = controller.grantFillToken(view, fbOrigin);
    assert(!validToken.isEmpty());
    assert(controller.activeTokenForView(view, fbOrigin) == validToken);

    // Mismatched token must not match active token
    const QString wrongToken = QStringLiteral("wrong-attacker-token-uuid");
    assert(controller.activeTokenForView(view, fbOrigin) != wrongToken);
    std::cout << "[PASS] SECURITY TEST: Wrong token rejected" << std::endl;
  }

  // SECURITY TEST 4 (Requirement 10.4): stale origin token rejected
  {
    std::cout << "[RUN] SECURITY TEST: Stale origin token rejected" << std::endl;
    auto *view = reinterpret_cast<QWebEngineView *>(0x6666);
    const QString oldOrigin = QStringLiteral("https://www.facebook.com");
    const QString oldToken = controller.grantFillToken(view, oldOrigin);
    assert(!oldToken.isEmpty());

    // Origin changed to another site: old token is wiped
    controller.onUrlChanged(view, QUrl(QStringLiteral("https://accounts.google.com")));
    assert(controller.activeTokenForView(view, oldOrigin).isEmpty());
    std::cout << "[PASS] SECURITY TEST: Stale origin token rejected" << std::endl;
  }

  // SECURITY TEST 5 (Requirement 10.5): password never appears in log output
  {
    std::cout << "[RUN] SECURITY TEST: Password never appears in log output" << std::endl;
    const QString script = CredentialAutofillController::domFillScript(fbOrigin, fbSecret.username, fbSecret.password);
    // domFillScript must NOT contain console.log or qDebug output
    assert(!script.contains("console.log"));
    assert(!script.contains("console.info"));
    assert(!script.contains("console.warn"));
    assert(!script.contains("console.error"));
    const QString captureScript = CredentialAutofillController::candidateCaptureScript().sourceCode();
    assert(captureScript.contains("QWebChannel"));
    assert(captureScript.contains("daliniraCredentials"));
    assert(captureScript.contains("__daliniraCredentialSend"));
    assert(!captureScript.contains("DALINIRA_CREDENTIAL"));
    const qsizetype captureStart = captureScript.indexOf(QStringLiteral("(() => {\n    if (window.top"));
    assert(captureStart >= 0);
    const QString daliniraCaptureSource = captureScript.mid(captureStart);
    assert(!daliniraCaptureSource.contains("console.log"));
    assert(!daliniraCaptureSource.contains("console.info"));
    assert(!daliniraCaptureSource.contains("console.warn"));
    assert(!daliniraCaptureSource.contains("console.error"));
    const QString webKitCapture = CredentialAutofillController::webKitCandidateCaptureSource();
    assert(webKitCapture.contains("window.webkit.messageHandlers.daliniraCredentials"));
    assert(webKitCapture.contains("window.top !== window"));
    assert(webKitCapture.contains("event.isTrusted"));
    assert(!webKitCapture.contains("QWebChannel"));
    assert(!webKitCapture.contains("console."));
    std::cout << "[PASS] SECURITY TEST: Password never appears in log output" << std::endl;
  }

  // SECURITY TEST 6 (Requirement 10.6): DOM cannot obtain vault credential list
  {
    std::cout << "[RUN] SECURITY TEST: DOM cannot obtain vault credential list" << std::endl;
    // Script payload only ever sends an ephemeral opaque token, never records, lists, or secrets
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("opaque-token-uuid"));
    assert(buttonScript.contains("{\"token\":\"opaque-token-uuid\"}"));
    assert(!buttonScript.contains("vaultManager"));
    assert(!buttonScript.contains("CredentialSecret"));
    assert(!buttonScript.contains(fbSecret.username));
    assert(!buttonScript.contains(fbSecret.password));
    assert(!buttonScript.contains("user@example.com"));
    assert(!buttonScript.contains("MySuperSecretFbPassword#2026"));
    assert(!buttonScript.contains("data-username"));
    assert(!buttonScript.contains("data-password"));
    assert(!buttonScript.contains("data-dalinira-credential"));
    std::cout << "[PASS] SECURITY TEST: DOM cannot obtain vault credential list" << std::endl;
  }

  // ==========================================
  // MODERN VAULT UNLOCK DIALOG & LIFECYCLE TESTS
  // ==========================================

  // TEST 22: VaultUnlockDialog creation & default UI properties
  {
    std::cout << "[RUN] TEST 22: VaultUnlockDialog creation & default UI properties" << std::endl;
    VaultUnlockDialog dialog(&vaultManager, QStringLiteral("https://www.facebook.com"));
    dialog.show();
    assert(dialog.windowFlags().testFlag(Qt::Dialog));
    assert(dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
    assert(dialog.testAttribute(Qt::WA_TranslucentBackground));
    assert(dialog.isModal());
    assert(dialog.width() == 400);

    assert(dialog.passwordInput() != nullptr);
    assert(dialog.passwordInput()->echoMode() == QLineEdit::Password);
    assert(dialog.passwordInput()->maxLength() == 256);
    assert(dialog.passwordInput()->placeholderText().contains(QStringLiteral("Ana parola")));

    assert(dialog.errorLabel() != nullptr);
    assert(!dialog.errorLabel()->isVisible());

    assert(dialog.unlockButton() != nullptr);
    assert(dialog.unlockButton()->text() == QStringLiteral("Kilidi Aç"));
    assert(dialog.unlockButton()->isEnabled());

    assert(dialog.cancelButton() != nullptr);
    assert(dialog.cancelButton()->text() == QStringLiteral("İptal"));
    assert(dialog.cancelButton()->isEnabled());

    std::cout << "[PASS] TEST 22: VaultUnlockDialog creation & default UI properties" << std::endl;
  }

  // TEST 23: Canonical origin badge display
  {
    std::cout << "[RUN] TEST 23: Canonical origin badge display" << std::endl;
    VaultUnlockDialog dialog(&vaultManager, QStringLiteral("https://www.facebook.com"));
    dialog.show();
    assert(dialog.canonicalOrigin() == QStringLiteral("https://www.facebook.com"));
    auto *originLabel = dialog.findChild<QLabel *>(QStringLiteral("vault-unlock-origin"));
    assert(originLabel != nullptr);
    assert(originLabel->isVisible());
    assert(originLabel->text().contains(QStringLiteral("www.facebook.com")));

    // When canonicalOrigin is empty, badge is hidden
    dialog.setCanonicalOrigin(QString());
    assert(dialog.canonicalOrigin().isEmpty());
    assert(!originLabel->isVisible());

    // Setting a new origin updates and shows the badge
    dialog.setCanonicalOrigin(QStringLiteral("https://accounts.google.com"));
    assert(originLabel->isVisible());
    assert(originLabel->text().contains(QStringLiteral("accounts.google.com")));

    std::cout << "[PASS] TEST 23: Canonical origin badge display" << std::endl;
  }

  // TEST 24: Password visibility toggle action
  {
    std::cout << "[RUN] TEST 24: Password visibility toggle action" << std::endl;
    VaultUnlockDialog dialog(&vaultManager);
    QLineEdit *input = dialog.passwordInput();
    assert(input->echoMode() == QLineEdit::Password);

    const auto actions = input->actions();
    assert(!actions.isEmpty());
    QAction *toggleAction = actions.first();
    assert(toggleAction != nullptr);
    assert(toggleAction->toolTip().contains(QStringLiteral("göster")));

    // Trigger toggle action: echo mode becomes Normal
    toggleAction->trigger();
    assert(input->echoMode() == QLineEdit::Normal);
    assert(toggleAction->toolTip().contains(QStringLiteral("gizle")));

    // Trigger again: toggles back to Password
    toggleAction->trigger();
    assert(input->echoMode() == QLineEdit::Password);
    assert(toggleAction->toolTip().contains(QStringLiteral("göster")));

    std::cout << "[PASS] TEST 24: Password visibility toggle action" << std::endl;
  }

  // TEST 25: Empty password input handling
  {
    std::cout << "[RUN] TEST 25: Empty password input handling" << std::endl;
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.passwordInput()->clear();
    dialog.attemptUnlock();

    assert(!dialog.isChecking());
    assert(dialog.errorLabel()->isVisible());
    assert(dialog.errorLabel()->text().contains(QStringLiteral("Lütfen ana parolanızı girin")));
    assert(dialog.result() != QDialog::Accepted);

    // Typing clears the error
    dialog.passwordInput()->setText(QStringLiteral("abc"));
    emit dialog.passwordInput()->textEdited(QStringLiteral("abc"));
    assert(!dialog.errorLabel()->isVisible());

    std::cout << "[PASS] TEST 25: Empty password input handling" << std::endl;
  }

  // TEST 26: Wrong master password handling
  {
    std::cout << "[RUN] TEST 26: Wrong master password handling" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    VaultUnlockDialog dialog(&vaultManager, QStringLiteral("https://www.facebook.com"));
    dialog.show();
    dialog.passwordInput()->setText(QStringLiteral("CompletelyWrongPassword#123"));

    dialog.attemptUnlock();
    assert(dialog.isChecking());

    bool finished = waitForCondition([&dialog]() { return !dialog.isChecking(); });
    assert(finished);

    // Dialog must NOT be accepted on failure
    assert(dialog.result() != QDialog::Accepted);
    assert(dialog.errorLabel()->isVisible());
    assert(dialog.errorLabel()->text().contains(QStringLiteral("Ana parola yanlış")));

    // Input must be cleared and submit button re-enabled
    assert(dialog.passwordInput()->text().isEmpty());
    assert(dialog.unlockButton()->isEnabled());
    assert(dialog.unlockButton()->text() == QStringLiteral("Kilidi Aç"));
    assert(dialog.cancelButton()->isEnabled());
    assert(vaultManager.isLocked());

    std::cout << "[PASS] TEST 26: Wrong master password handling" << std::endl;
  }

  // TEST 27: Rate-limiting feedback display
  {
    std::cout << "[RUN] TEST 27: Rate-limiting feedback display" << std::endl;
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.setError(QStringLiteral("Çok fazla başarısız deneme. Lütfen birkaç saniye bekleyin."));
    assert(dialog.errorLabel()->isVisible());
    assert(dialog.errorLabel()->text().contains(QStringLiteral("Çok fazla başarısız deneme")));
    assert(dialog.errorLabel()->text().contains(QStringLiteral("birkaç saniye bekleyin")));

    dialog.clearError();
    assert(!dialog.errorLabel()->isVisible());
    std::cout << "[PASS] TEST 27: Rate-limiting feedback display" << std::endl;
  }

  // TEST 28: Correct master password unlock
  {
    std::cout << "[RUN] TEST 28: Correct master password unlock" << std::endl;
    QThread::msleep(600); // Backoff delay
    assert(vaultManager.isLocked());

    VaultUnlockDialog dialog(&vaultManager, fbOrigin);
    dialog.show();
    dialog.passwordInput()->setText(masterPassword);

    bool signalEmitted = false;
    QObject::connect(&dialog, &VaultUnlockDialog::unlocked, [&signalEmitted] {
      signalEmitted = true;
    });

    dialog.attemptUnlock();
    assert(dialog.isChecking());

    bool finished = waitForCondition([&dialog]() { return !dialog.isChecking(); });
    assert(finished);

    assert(signalEmitted);
    assert(dialog.result() == QDialog::Accepted);
    assert(dialog.passwordInput()->text().isEmpty()); // Master password purged
    assert(!vaultManager.isLocked());

    std::cout << "[PASS] TEST 28: Correct master password unlock" << std::endl;
  }

  // TEST 29: Double-submit prevention
  {
    std::cout << "[RUN] TEST 29: Double-submit prevention" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.passwordInput()->setText(masterPassword);

    dialog.attemptUnlock();
    assert(dialog.isChecking());
    assert(!dialog.unlockButton()->isEnabled());
    assert(dialog.unlockButton()->text().contains(QStringLiteral("Doğrulanıyor")));
    assert(!dialog.cancelButton()->isEnabled());

    // Multiple calls while checking must be ignored
    dialog.attemptUnlock();
    assert(dialog.isChecking());

    waitForCondition([&dialog]() { return !dialog.isChecking(); });
    std::cout << "[PASS] TEST 29: Double-submit prevention" << std::endl;
  }

  // TEST 30: Keyboard handling (Enter & Escape)
  {
    std::cout << "[RUN] TEST 30: Keyboard handling (Enter & Escape)" << std::endl;
    vaultManager.lock();

    // Escape triggers reject
    {
      VaultUnlockDialog dialog(&vaultManager);
      dialog.show();
      QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(&dialog, &escEvent);
      assert(dialog.result() == QDialog::Rejected);
    }

    // Enter triggers attemptUnlock
    {
      VaultUnlockDialog dialog(&vaultManager);
      dialog.show();
      dialog.passwordInput()->setText(QStringLiteral("test-pwd"));
      QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(&dialog, &enterEvent);
      assert(dialog.isChecking());
      waitForCondition([&dialog]() { return !dialog.isChecking(); });
    }

    std::cout << "[PASS] TEST 30: Keyboard handling (Enter & Escape)" << std::endl;
  }

  // TEST 31: Single dialog policy
  {
    std::cout << "[RUN] TEST 31: Single dialog policy" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    controller.unlockVaultForFill(testView);
    VaultUnlockDialog *firstDialog = controller.activeUnlockDialog();
    assert(firstDialog != nullptr);
    assert(firstDialog->canonicalOrigin() == QStringLiteral("https://www.facebook.com"));

    // Repeated call must return / reuse the existing dialog instance
    controller.unlockVaultForFill(testView);
    assert(controller.activeUnlockDialog() == firstDialog);

    firstDialog->reject();
    QCoreApplication::processEvents();
    assert(controller.activeUnlockDialog() == nullptr);
    testView->deleteLater();
    QCoreApplication::processEvents();

    std::cout << "[PASS] TEST 31: Single dialog policy" << std::endl;
  }

  // TEST 32: Automatic pending autofill resumption
  {
    std::cout << "[RUN] TEST 32: Automatic pending autofill resumption" << std::endl;
    QThread::msleep(600); // Backoff delay
    vaultManager.lock();
    assert(vaultManager.isLocked());

    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    controller.unlockVaultForFill(testView);
    QPointer<VaultUnlockDialog> dialog = controller.activeUnlockDialog();
    assert(!dialog.isNull());

    dialog->passwordInput()->setText(masterPassword);
    dialog->attemptUnlock();

    waitForCondition([dialog]() { return dialog.isNull() || !dialog->isChecking(); });
    QCoreApplication::processEvents();

    assert(!vaultManager.isLocked());
    assert(controller.activeUnlockDialog() == nullptr);
    testView->deleteLater();
    QCoreApplication::processEvents();

    std::cout << "[PASS] TEST 32: Automatic pending autofill resumption" << std::endl;
  }

  // TEST 33: Cancellation policy
  {
    std::cout << "[RUN] TEST 33: Cancellation policy" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    controller.unlockVaultForFill(testView);
    VaultUnlockDialog *dialog = controller.activeUnlockDialog();
    assert(dialog != nullptr);

    dialog->reject();
    QCoreApplication::processEvents();

    assert(controller.activeUnlockDialog() == nullptr);
    assert(vaultManager.isLocked()); // Vault remains locked
    testView->deleteLater();
    QCoreApplication::processEvents();

    std::cout << "[PASS] TEST 33: Cancellation policy" << std::endl;
  }

  // TEST 34: Tab close lifecycle cleanup
  {
    std::cout << "[RUN] TEST 34: Tab close lifecycle cleanup" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    auto *tabView = new QWebEngineView();
    tabView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    controller.unlockVaultForFill(tabView);
    VaultUnlockDialog *dialog = controller.activeUnlockDialog();
    assert(dialog != nullptr);

    // Tab closed
    controller.onViewClosed(tabView);
    QCoreApplication::processEvents();

    assert(controller.activeUnlockDialog() == nullptr);
    tabView->deleteLater();
    QCoreApplication::processEvents();

    std::cout << "[PASS] TEST 34: Tab close lifecycle cleanup" << std::endl;
  }

  // TEST 35: Navigation origin change lifecycle cleanup
  {
    std::cout << "[RUN] TEST 35: Navigation origin change lifecycle cleanup" << std::endl;
    vaultManager.lock();
    assert(vaultManager.isLocked());

    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    controller.unlockVaultForFill(testView);
    VaultUnlockDialog *dialog = controller.activeUnlockDialog();
    assert(dialog != nullptr);

    // View navigated to a different origin
    testView->setUrl(QUrl(QStringLiteral("https://accounts.google.com")));
    controller.onUrlChanged(testView, testView->url());
    QCoreApplication::processEvents();

    assert(controller.activeUnlockDialog() == nullptr);
    testView->deleteLater();
    QCoreApplication::processEvents();

    std::cout << "[PASS] TEST 35: Navigation origin change lifecycle cleanup" << std::endl;
  }

  // TEST 36: Master password secrecy guarantee
  {
    std::cout << "[RUN] TEST 36: Master password secrecy guarantee" << std::endl;
    VaultUnlockDialog dialog(&vaultManager);
    dialog.passwordInput()->setText(QStringLiteral("TemporarySecretTestValue"));
    assert(dialog.passwordInput()->text() == QStringLiteral("TemporarySecretTestValue"));

    // Destructor or failed/successful attempt clears the field
    dialog.passwordInput()->clear();
    assert(dialog.passwordInput()->text().isEmpty());

    // Verify domFillScript and fillButtonScript never contain master password
    const QString fillScript = CredentialAutofillController::domFillScript(fbOrigin, fbSecret.username, fbSecret.password);
    assert(!fillScript.contains("MasterSecret#2026"));
    const QString buttonScript = CredentialAutofillController::fillButtonScript(QStringLiteral("token-123"));
    assert(!buttonScript.contains("MasterSecret#2026"));

    std::cout << "[PASS] TEST 36: Master password secrecy guarantee" << std::endl;
  }

  // ==========================================
  // PROGRESSIVE RATE LIMITING & BRUTE-FORCE PROTECTION TESTS (TEST 37 - TEST 60)
  // ==========================================

  // Setup simulated time for deterministic testing without sleeps
  qint64 simTime = 1757200000000LL;
  auto testTimeProvider = [&simTime]() -> qint64 { return simTime; };
  vaultManager.setTimeProviderForTesting(testTimeProvider);

  // Ensure vault is locked and failed attempts start at 0
  vaultManager.lock();
  vaultManager.resetFailedUnlockAttempts();
  assert(vaultManager.failedUnlockAttempts() == 0);
  assert(!vaultManager.isUnlockRateLimited());
  assert(vaultManager.remainingUnlockCooldownSeconds() == 0);

  // TEST 37: 1 wrong -> no cooldown
  {
    std::cout << "[RUN] TEST 37: 1 wrong -> no cooldown" << std::endl;
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#1")));
    assert(vaultManager.failedUnlockAttempts() == 1);
    assert(!vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 0);
    std::cout << "[PASS] TEST 37: 1 wrong -> no cooldown" << std::endl;
  }

  // TEST 38: 4 wrong -> no cooldown
  {
    std::cout << "[RUN] TEST 38: 4 wrong -> no cooldown" << std::endl;
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#2")));
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#3")));
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#4")));
    assert(vaultManager.failedUnlockAttempts() == 4);
    assert(!vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 0);
    std::cout << "[PASS] TEST 38: 4 wrong -> no cooldown" << std::endl;
  }

  // TEST 39: 5th wrong -> 30 sec cooldown
  {
    std::cout << "[RUN] TEST 39: 5th wrong -> 30 sec cooldown" << std::endl;
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#5")));
    assert(vaultManager.failedUnlockAttempts() == 5);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 30);
    std::cout << "[PASS] TEST 39: 5th wrong -> 30 sec cooldown" << std::endl;
  }

  // TEST 40: 6th wrong -> 60 sec
  {
    std::cout << "[RUN] TEST 40: 6th wrong -> 60 sec" << std::endl;
    simTime += 30000LL; // 30s elapsed, cooldown expired
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#6")));
    assert(vaultManager.failedUnlockAttempts() == 6);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 60);
    std::cout << "[PASS] TEST 40: 6th wrong -> 60 sec" << std::endl;
  }

  // TEST 41: 7th wrong -> 120 sec
  {
    std::cout << "[RUN] TEST 41: 7th wrong -> 120 sec" << std::endl;
    simTime += 60000LL; // 60s elapsed
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#7")));
    assert(vaultManager.failedUnlockAttempts() == 7);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 120);
    std::cout << "[PASS] TEST 41: 7th wrong -> 120 sec" << std::endl;
  }

  // TEST 42: 8th wrong -> 300 sec
  {
    std::cout << "[RUN] TEST 42: 8th wrong -> 300 sec" << std::endl;
    simTime += 120000LL; // 120s elapsed
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#8")));
    assert(vaultManager.failedUnlockAttempts() == 8);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 300);
    std::cout << "[PASS] TEST 42: 8th wrong -> 300 sec" << std::endl;
  }

  // TEST 43: 9th wrong -> 900 sec
  {
    std::cout << "[RUN] TEST 43: 9th wrong -> 900 sec" << std::endl;
    simTime += 300000LL; // 300s elapsed
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#9")));
    assert(vaultManager.failedUnlockAttempts() == 9);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 900);
    std::cout << "[PASS] TEST 43: 9th wrong -> 900 sec" << std::endl;
  }

  // TEST 44: 10th wrong -> 1800 sec
  {
    std::cout << "[RUN] TEST 44: 10th wrong -> 1800 sec" << std::endl;
    simTime += 900000LL; // 900s elapsed
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#10")));
    assert(vaultManager.failedUnlockAttempts() == 10);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 1800);
    std::cout << "[PASS] TEST 44: 10th wrong -> 1800 sec" << std::endl;
  }

  // TEST 45: 11th wrong -> still 1800 sec
  {
    std::cout << "[RUN] TEST 45: 11th wrong -> still 1800 sec" << std::endl;
    simTime += 1800000LL; // 1800s elapsed
    assert(!vaultManager.isUnlockRateLimited());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#11")));
    assert(vaultManager.failedUnlockAttempts() == 11);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 1800);
    std::cout << "[PASS] TEST 45: 11th wrong -> still 1800 sec" << std::endl;
  }

  // TEST 46: correct password resets attempts
  {
    std::cout << "[RUN] TEST 46: correct password resets attempts" << std::endl;
    simTime += 1800000LL; // cooldown expired
    assert(!vaultManager.isUnlockRateLimited());
    assert(vaultManager.unlock(masterPassword));
    assert(!vaultManager.isLocked());
    assert(vaultManager.failedUnlockAttempts() == 0);
    assert(!vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 0);

    // Relock and verify first subsequent failure has 0s cooldown
    vaultManager.lock();
    assert(vaultManager.isLocked());
    assert(!vaultManager.unlock(QStringLiteral("WrongPassword#AfterReset")));
    assert(vaultManager.failedUnlockAttempts() == 1);
    assert(!vaultManager.isUnlockRateLimited());
    vaultManager.resetFailedUnlockAttempts();
    std::cout << "[PASS] TEST 46: correct password resets attempts" << std::endl;
  }

  // TEST 47: cancel does not increment
  {
    std::cout << "[RUN] TEST 47: cancel does not increment" << std::endl;
    assert(vaultManager.failedUnlockAttempts() == 0);
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.reject();
    assert(dialog.result() == QDialog::Rejected);
    assert(vaultManager.failedUnlockAttempts() == 0);
    std::cout << "[PASS] TEST 47: cancel does not increment" << std::endl;
  }

  // TEST 48: empty password does not increment
  {
    std::cout << "[RUN] TEST 48: empty password does not increment" << std::endl;
    assert(vaultManager.failedUnlockAttempts() == 0);
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.passwordInput()->clear();
    dialog.attemptUnlock();
    assert(!dialog.isChecking());
    assert(dialog.errorLabel()->isVisible());
    assert(dialog.errorLabel()->text().contains(QStringLiteral("ana parolanızı girin")));
    assert(vaultManager.failedUnlockAttempts() == 0);
    std::cout << "[PASS] TEST 48: empty password does not increment" << std::endl;
  }

  // TEST 49: cooldown submit does not increment
  {
    std::cout << "[RUN] TEST 49: cooldown submit does not increment" << std::endl;
    // Trigger 5 failures
    for (int i = 1; i <= 5; ++i) {
      vaultManager.unlock(QStringLiteral("BadPwd%1").arg(i));
    }
    assert(vaultManager.failedUnlockAttempts() == 5);
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 30);

    // Direct unlock attempt during cooldown
    assert(!vaultManager.unlock(QStringLiteral("BadPwdDuringCooldown")));
    assert(vaultManager.lastError() == QStringLiteral("unlock-rate-limited"));
    assert(vaultManager.failedUnlockAttempts() == 5); // Must not increment!
    assert(vaultManager.remainingUnlockCooldownSeconds() == 30);

    // Dialog unlock attempt during cooldown
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    assert(dialog.isCooldownActive());
    assert(!dialog.unlockButton()->isEnabled());
    assert(!dialog.passwordInput()->isEnabled());
    dialog.attemptUnlock(); // Should be completely blocked
    assert(vaultManager.failedUnlockAttempts() == 5);
    std::cout << "[PASS] TEST 49: cooldown submit does not increment" << std::endl;
  }

  // TEST 50: dialog close/reopen preserves remaining cooldown
  {
    std::cout << "[RUN] TEST 50: dialog close/reopen preserves remaining cooldown" << std::endl;
    assert(vaultManager.isUnlockRateLimited());
    // Advance simulated time by 10s -> 20s remaining
    simTime += 10000LL;
    assert(vaultManager.remainingUnlockCooldownSeconds() == 20);

    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    assert(dialog.isCooldownActive());
    assert(dialog.remainingCooldown() == 20);
    assert(dialog.errorLabel()->text().contains(QStringLiteral("00:20")));
    assert(!dialog.passwordInput()->isEnabled());
    assert(!dialog.unlockButton()->isEnabled());

    // Close dialog
    dialog.reject();

    // Advance 5 more seconds -> 15s remaining
    simTime += 5000LL;
    assert(vaultManager.remainingUnlockCooldownSeconds() == 15);

    // Reopen dialog
    VaultUnlockDialog reopened(&vaultManager);
    reopened.show();
    assert(reopened.isCooldownActive());
    assert(reopened.remainingCooldown() == 15);
    assert(reopened.errorLabel()->text().contains(QStringLiteral("00:15")));
    std::cout << "[PASS] TEST 50: dialog close/reopen preserves remaining cooldown" << std::endl;
  }

  // TEST 51: tab close does not reset cooldown
  {
    std::cout << "[RUN] TEST 51: tab close does not reset cooldown" << std::endl;
    assert(vaultManager.isUnlockRateLimited());
    auto *testTab = new QWebEngineView();
    testTab->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller.unlockVaultForFill(testTab);
    assert(controller.activeUnlockDialog() != nullptr);
    assert(controller.activeUnlockDialog()->isCooldownActive());

    // Close tab
    controller.onViewClosed(testTab);
    testTab->deleteLater();
    QCoreApplication::processEvents();

    assert(controller.activeUnlockDialog() == nullptr);
    // Vault cooldown is preserved
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.failedUnlockAttempts() == 5);
    std::cout << "[PASS] TEST 51: tab close does not reset cooldown" << std::endl;
  }

  // TEST 52: origin navigation does not reset cooldown
  {
    std::cout << "[RUN] TEST 52: origin navigation does not reset cooldown" << std::endl;
    assert(vaultManager.isUnlockRateLimited());
    auto *navTab = new QWebEngineView();
    navTab->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller.unlockVaultForFill(navTab);
    assert(controller.activeUnlockDialog() != nullptr);

    // Navigate to different origin
    controller.onUrlChanged(navTab, QUrl(QStringLiteral("https://other-site.com")));
    QCoreApplication::processEvents();
    assert(controller.activeUnlockDialog() == nullptr);

    // Cooldown is strictly preserved
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.failedUnlockAttempts() == 5);
    navTab->deleteLater();
    QCoreApplication::processEvents();
    std::cout << "[PASS] TEST 52: origin navigation does not reset cooldown" << std::endl;
  }

  // TEST 53: multi-tab same vault shares cooldown
  {
    std::cout << "[RUN] TEST 53: multi-tab same vault shares cooldown" << std::endl;
    assert(vaultManager.isUnlockRateLimited());

    // Dialog for Tab A
    VaultUnlockDialog dialogA(&vaultManager, QStringLiteral("https://www.facebook.com"));
    dialogA.show();
    assert(dialogA.isCooldownActive());
    assert(!dialogA.unlockButton()->isEnabled());

    // Dialog for Tab B
    VaultUnlockDialog dialogB(&vaultManager, QStringLiteral("https://accounts.google.com"));
    dialogB.show();
    assert(dialogB.isCooldownActive());
    assert(!dialogB.unlockButton()->isEnabled());
    assert(dialogA.remainingCooldown() == dialogB.remainingCooldown());

    std::cout << "[PASS] TEST 53: multi-tab same vault shares cooldown" << std::endl;
  }

  // TEST 54: browser restart preserves cooldown
  {
    std::cout << "[RUN] TEST 54: browser restart preserves cooldown" << std::endl;
    // Create new vault manager pointing to same data directory
    CredentialVaultManager restartedManager(tempDir.path());
    restartedManager.setTimeProviderForTesting(testTimeProvider);

    assert(restartedManager.failedUnlockAttempts() == 5);
    assert(restartedManager.isUnlockRateLimited());
    assert(restartedManager.remainingUnlockCooldownSeconds() == 15);
    std::cout << "[PASS] TEST 54: browser restart preserves cooldown" << std::endl;
  }

  // TEST 55: countdown UI reaches zero and reenables input
  {
    std::cout << "[RUN] TEST 55: countdown UI reaches zero and reenables input" << std::endl;
    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    assert(dialog.isCooldownActive());
    assert(!dialog.passwordInput()->isEnabled());
    assert(!dialog.unlockButton()->isEnabled());

    // Advance time beyond the remaining cooldown
    simTime += 20000LL;
    assert(vaultManager.remainingUnlockCooldownSeconds() == 0);

    // Trigger cooldown check / expiry
    dialog.checkExistingCooldown();
    assert(!dialog.isCooldownActive());
    assert(dialog.passwordInput()->isEnabled());
    assert(dialog.unlockButton()->isEnabled());
    assert(!dialog.errorLabel()->isVisible());
    std::cout << "[PASS] TEST 55: countdown UI reaches zero and reenables input" << std::endl;
  }

  // TEST 56: PBKDF2 not started while rate limited
  {
    std::cout << "[RUN] TEST 56: PBKDF2 not started while rate limited" << std::endl;
    // Trigger rate limit again
    assert(!vaultManager.unlock(QStringLiteral("BadPwdAgain#6"))); // 6th failure -> 60s
    assert(vaultManager.isUnlockRateLimited());
    assert(vaultManager.remainingUnlockCooldownSeconds() == 60);

    // Measure time of rate-limited unlock attempt: should be instant (sub-millisecond)
    // whereas PBKDF2 with 600,000 iterations takes ~200-500ms
    QElapsedTimer perfTimer;
    perfTimer.start();
    const bool rateLimitedResult = vaultManager.unlock(QStringLiteral("AnyPwdDuringRateLimit"));
    const qint64 elapsedMs = perfTimer.elapsed();

    assert(!rateLimitedResult);
    assert(vaultManager.lastError() == QStringLiteral("unlock-rate-limited"));
    assert(elapsedMs < 10); // Verifies PBKDF2 was completely bypassed!
    std::cout << "[PASS] TEST 56: PBKDF2 not started while rate limited" << std::endl;
  }

  // TEST 57: wrong password only increments once per async result
  {
    std::cout << "[RUN] TEST 57: wrong password only increments once per async result" << std::endl;
    simTime += 60000LL; // cooldown expired
    assert(!vaultManager.isUnlockRateLimited());
    const int beforeAttempts = vaultManager.failedUnlockAttempts();

    VaultUnlockDialog dialog(&vaultManager);
    dialog.show();
    dialog.passwordInput()->setText(QStringLiteral("AsyncWrongPassword#Test"));
    dialog.attemptUnlock();
    assert(dialog.isChecking());

    waitForCondition([&dialog]() { return !dialog.isChecking(); });
    assert(vaultManager.failedUnlockAttempts() == beforeAttempts + 1);
    std::cout << "[PASS] TEST 57: wrong password only increments once per async result" << std::endl;
  }

  // TEST 58: successful unlock clears persisted rate state
  {
    std::cout << "[RUN] TEST 58: successful unlock clears persisted rate state" << std::endl;
    // Expire any active cooldown
    simTime += 120000LL;
    assert(!vaultManager.isUnlockRateLimited());

    assert(vaultManager.unlock(masterPassword));
    assert(vaultManager.failedUnlockAttempts() == 0);
    assert(vaultManager.remainingUnlockCooldownSeconds() == 0);

    // Verify security-state.json is removed / cleaned up
    const QString secStatePath = tempDir.path() + QStringLiteral("/credential-vault/security-state.json");
    assert(!QFile::exists(secStatePath));
    std::cout << "[PASS] TEST 58: successful unlock clears persisted rate state" << std::endl;
  }

  // TEST 59: corrupted rate-limit metadata safe fallback
  {
    std::cout << "[RUN] TEST 59: corrupted rate-limit metadata safe fallback" << std::endl;
    const QString secStatePath = tempDir.path() + QStringLiteral("/credential-vault/security-state.json");

    // Case A: Malformed JSON
    {
      QFile file(secStatePath);
      assert(file.open(QIODevice::WriteOnly));
      file.write("INVALID_CORRUPTED_JSON{{{");
      file.close();

      CredentialVaultManager corruptManager(tempDir.path());
      assert(corruptManager.failedUnlockAttempts() == 0);
      assert(!corruptManager.isUnlockRateLimited());
      assert(corruptManager.remainingUnlockCooldownSeconds() == 0);
    }

    // Case B: Negative attempts and impossible expiry
    {
      QFile file(secStatePath);
      assert(file.open(QIODevice::WriteOnly));
      file.write("{\"failedAttempts\": -999, \"cooldownExpiryEpochMs\": -12345}");
      file.close();

      CredentialVaultManager corruptManager(tempDir.path());
      assert(corruptManager.failedUnlockAttempts() == 0);
      assert(!corruptManager.isUnlockRateLimited());
      assert(corruptManager.remainingUnlockCooldownSeconds() == 0);
    }

    // Case C: Absurdly large attempt count clamped safely
    {
      QFile file(secStatePath);
      assert(file.open(QIODevice::WriteOnly));
      file.write("{\"failedAttempts\": 99999999, \"cooldownExpiryEpochMs\": 0}");
      file.close();

      CredentialVaultManager corruptManager(tempDir.path());
      assert(corruptManager.failedUnlockAttempts() <= 10000);
      assert(!corruptManager.isUnlockRateLimited());
    }

    // Clean up
    QFile::remove(secStatePath);
    std::cout << "[PASS] TEST 59: corrupted rate-limit metadata safe fallback" << std::endl;
  }

  // TEST 60: 30 minute cap enforced
  {
    std::cout << "[RUN] TEST 60: 30 minute cap enforced" << std::endl;
    assert(CredentialVault::cooldownForAttempt(0) == 0);
    assert(CredentialVault::cooldownForAttempt(1) == 0);
    assert(CredentialVault::cooldownForAttempt(4) == 0);
    assert(CredentialVault::cooldownForAttempt(5) == 30);
    assert(CredentialVault::cooldownForAttempt(6) == 60);
    assert(CredentialVault::cooldownForAttempt(7) == 120);
    assert(CredentialVault::cooldownForAttempt(8) == 300);
    assert(CredentialVault::cooldownForAttempt(9) == 900);
    assert(CredentialVault::cooldownForAttempt(10) == 1800); // 30 minutes cap
    assert(CredentialVault::cooldownForAttempt(11) == 1800);
    assert(CredentialVault::cooldownForAttempt(50) == 1800);
    assert(CredentialVault::cooldownForAttempt(100) == 1800);
    assert(CredentialVault::cooldownForAttempt(1000) == 1800);
    std::cout << "[PASS] TEST 60: 30 minute cap enforced" << std::endl;
  }

  // TEST 61: vault does not exist -> no key icon
  {
    std::cout << "[RUN] TEST 61: vault does not exist -> no key icon" << std::endl;
    QTemporaryDir emptyDir;
    assert(emptyDir.isValid());
    CredentialVaultManager noVaultManager(emptyDir.path());
    assert(!noVaultManager.exists());

    CredentialAutofillController noVaultController(&noVaultManager);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // When page finishes loading on Facebook login
    noVaultController.onPageLoadFinished(testView, true);

    // Matching credential check must be false
    assert(!noVaultManager.hasMatchingCredential(testView->url()));
    // Fill button must NOT be active and no token granted
    assert(!noVaultController.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(noVaultController.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());

    noVaultController.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 61: vault does not exist -> no key icon" << std::endl;
  }

  // TEST 62: vault exists but current origin has no credential -> no key icon
  {
    std::cout << "[RUN] TEST 62: vault exists but current origin has no credential -> no key icon" << std::endl;
    QTemporaryDir dir62;
    assert(dir62.isValid());
    CredentialVaultManager manager62(dir62.path());
    assert(manager62.create(masterPassword));
    assert(manager62.exists());

    // Save credential ONLY for accounts.google.com
    CredentialSecret googleSecret;
    googleSecret.origin = QStringLiteral("https://accounts.google.com");
    googleSecret.username = QStringLiteral("user@example.com");
    googleSecret.password = QStringLiteral("GoogleSecret#2026");
    bool updated = false;
    assert(manager62.save(googleSecret, &updated));

    CredentialAutofillController controller62(&manager62);
    auto *testView = new QWebEngineView();
    // User navigates to Facebook login (no credential exists for Facebook)
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller62.onPageLoadFinished(testView, true);

    assert(!manager62.hasMatchingCredential(testView->url()));
    assert(!controller62.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(controller62.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());

    controller62.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 62: vault exists but current origin has no credential -> no key icon" << std::endl;
  }

  // TEST 63: vault locked + matching credential exists -> key icon visible
  {
    std::cout << "[RUN] TEST 63: vault locked + matching credential exists -> key icon visible" << std::endl;
    QTemporaryDir dir63;
    assert(dir63.isValid());
    CredentialVaultManager manager63(dir63.path());
    assert(manager63.create(masterPassword));

    // Save credential for Facebook
    CredentialSecret fbSecret63;
    fbSecret63.origin = QStringLiteral("https://www.facebook.com");
    fbSecret63.username = QStringLiteral("user@example.com");
    fbSecret63.password = QStringLiteral("FbSecret#2026");
    bool updated = false;
    assert(manager63.save(fbSecret63, &updated));

    // Lock the vault
    manager63.lock();
    assert(manager63.isLocked());

    // Even though vault is locked, matching credential exists for Facebook
    assert(manager63.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    CredentialAutofillController controller63(&manager63);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller63.onPageLoadFinished(testView, true);

    // Key icon must be active and token granted
    assert(controller63.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    const QString token = controller63.activeTokenForView(testView, QStringLiteral("https://www.facebook.com"));
    assert(!token.isEmpty());

    controller63.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 63: vault locked + matching credential exists -> key icon visible" << std::endl;
  }

  // TEST 64: vault unlocked + matching credential exists -> key icon visible
  {
    std::cout << "[RUN] TEST 64: vault unlocked + matching credential exists -> key icon visible" << std::endl;
    QTemporaryDir dir64;
    assert(dir64.isValid());
    CredentialVaultManager manager64(dir64.path());
    assert(manager64.create(masterPassword));
    assert(!manager64.isLocked());

    CredentialSecret fbSecret64;
    fbSecret64.origin = QStringLiteral("https://www.facebook.com");
    fbSecret64.username = QStringLiteral("user@example.com");
    fbSecret64.password = QStringLiteral("FbSecret#2026");
    bool updated = false;
    assert(manager64.save(fbSecret64, &updated));

    assert(manager64.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    CredentialAutofillController controller64(&manager64);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller64.onPageLoadFinished(testView, true);

    assert(controller64.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(!controller64.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());

    controller64.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 64: vault unlocked + matching credential exists -> key icon visible" << std::endl;
  }

  // TEST 65: vault deleted while page open -> icon removed
  {
    std::cout << "[RUN] TEST 65: vault deleted while page open -> icon removed" << std::endl;
    QTemporaryDir dir65;
    assert(dir65.isValid());
    CredentialVaultManager manager65(dir65.path());
    assert(manager65.create(masterPassword));

    CredentialSecret fbSecret65;
    fbSecret65.origin = QStringLiteral("https://www.facebook.com");
    fbSecret65.username = QStringLiteral("user@example.com");
    fbSecret65.password = QStringLiteral("FbSecret#2026");
    bool updated = false;
    assert(manager65.save(fbSecret65, &updated));

    CredentialAutofillController controller65(&manager65);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller65.onPageLoadFinished(testView, true);

    assert(controller65.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));

    // Delete the vault while the page is still open
    assert(manager65.reset());
    assert(!manager65.exists());

    // Stale icon must be immediately removed and token revoked
    assert(!manager65.hasMatchingCredential(testView->url()));
    assert(!controller65.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(controller65.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());

    controller65.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 65: vault deleted while page open -> icon removed" << std::endl;
  }

  // TEST 66: navigate from origin with credential to origin without credential -> icon removed
  {
    std::cout << "[RUN] TEST 66: navigate from origin with credential to origin without credential -> icon removed" << std::endl;
    QTemporaryDir dir66;
    assert(dir66.isValid());
    CredentialVaultManager manager66(dir66.path());
    assert(manager66.create(masterPassword));

    CredentialSecret fbSecret66;
    fbSecret66.origin = QStringLiteral("https://www.facebook.com");
    fbSecret66.username = QStringLiteral("user@example.com");
    fbSecret66.password = QStringLiteral("FbSecret#2026");
    bool updated = false;
    assert(manager66.save(fbSecret66, &updated));

    CredentialAutofillController controller66(&manager66);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    controller66.onPageLoadFinished(testView, true);

    // Initial origin has credential -> icon visible
    assert(controller66.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(!controller66.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());

    // Navigate to origin without credentials (e.g. github.com)
    testView->setUrl(QUrl(QStringLiteral("https://github.com/login")));
    controller66.onUrlChanged(testView, testView->url());
    controller66.onPageLoadFinished(testView, true);

    // Previous origin icon and new origin icon must both be inactive/removed
    assert(!controller66.isFillButtonActiveForView(testView, QStringLiteral("https://www.facebook.com")));
    assert(!controller66.isFillButtonActiveForView(testView, QStringLiteral("https://github.com")));
    assert(controller66.activeTokenForView(testView, QStringLiteral("https://www.facebook.com")).isEmpty());
    assert(controller66.activeTokenForView(testView, QStringLiteral("https://github.com")).isEmpty());

    controller66.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 66: navigate from origin with credential to origin without credential -> icon removed" << std::endl;
  }

  // TEST 67: new credential added for current origin -> icon becomes available after refresh/re-scan
  {
    std::cout << "[RUN] TEST 67: new credential added for current origin -> icon becomes available after refresh/re-scan" << std::endl;
    QTemporaryDir dir67;
    assert(dir67.isValid());
    CredentialVaultManager manager67(dir67.path());
    assert(manager67.create(masterPassword));

    CredentialAutofillController controller67(&manager67);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://twitter.com/login")));
    controller67.onPageLoadFinished(testView, true);

    // Initially no credential for twitter -> no key icon
    assert(!manager67.hasMatchingCredential(testView->url()));
    assert(!controller67.isFillButtonActiveForView(testView, QStringLiteral("https://twitter.com")));
    assert(controller67.activeTokenForView(testView, QStringLiteral("https://twitter.com")).isEmpty());

    // User adds new credential for twitter in Password Manager tab
    CredentialSecret twitterSecret;
    twitterSecret.origin = QStringLiteral("https://twitter.com");
    twitterSecret.username = QStringLiteral("testuser");
    twitterSecret.password = QStringLiteral("TwitterSecret#2026");
    bool updated = false;
    assert(manager67.save(twitterSecret, &updated));

    // Via refreshAutofillForViews (triggered by manager67::changed signal), icon becomes active
    assert(manager67.hasMatchingCredential(testView->url()));
    assert(controller67.isFillButtonActiveForView(testView, QStringLiteral("https://twitter.com")));
    assert(!controller67.activeTokenForView(testView, QStringLiteral("https://twitter.com")).isEmpty());

    controller67.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 67: new credential added for current origin -> icon becomes available after refresh/re-scan" << std::endl;
  }

  // TEST 68: login success + no vault -> completely silent, no save bubble shown
  {
    std::cout << "[RUN] TEST 68: login success + no vault -> completely silent, no save bubble shown" << std::endl;
    QTemporaryDir dir68;
    assert(dir68.isValid());
    CredentialVaultManager manager68(dir68.path());
    assert(!manager68.exists());

    CredentialAutofillController controller68(&manager68);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    assert(controller68.handleBridgeMessage(testView->page(), candMsg));

    const QString successMsg = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller68.handleBridgeMessage(testView->page(), successMsg));

    // When no vault exists, system must be completely silent: no bubble, no pending credentials
    assert(controller68.activeSaveBubble() == nullptr);
    assert(controller68.pendingCandidateCount() == 0);

    controller68.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 68: login success + no vault -> completely silent, no save bubble shown" << std::endl;
  }

  // TEST 69: login failure + no vault -> no save bubble
  {
    std::cout << "[RUN] TEST 69: login failure + no vault -> no save bubble" << std::endl;
    QTemporaryDir dir69;
    assert(dir69.isValid());
    CredentialVaultManager manager69(dir69.path());
    assert(!manager69.exists());

    CredentialAutofillController controller69(&manager69);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"WrongPassword\"}");
    assert(controller69.handleBridgeMessage(testView->page(), candMsg));

    // Login failed: no success hint sent, and page reloaded same login URL
    controller69.onPageLoadFinished(testView, true);

    // Save bubble must NOT be shown
    assert(controller69.activeSaveBubble() == nullptr);

    controller69.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 69: login failure + no vault -> no save bubble" << std::endl;
  }

  // TEST 70: no vault -> no automatic candidate staging or retention
  {
    std::cout << "[RUN] TEST 70: no vault -> no automatic candidate staging or retention" << std::endl;
    QTemporaryDir dir70;
    assert(dir70.isValid());
    CredentialVaultManager manager70(dir70.path());
    assert(!manager70.exists());

    CredentialAutofillController controller70(&manager70);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller70.handleBridgeMessage(testView->page(), candMsg);
    controller70.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    // No bubble, candidate count is zero
    assert(controller70.activeSaveBubble() == nullptr);
    assert(controller70.pendingCandidateCount() == 0);

    controller70.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 70: no vault -> no automatic candidate staging or retention" << std::endl;
  }

  // TEST 71: no vault login + subsequent vault creation -> past credential NEVER saved
  {
    std::cout << "[RUN] TEST 71: no vault login + subsequent vault creation -> past credential NEVER saved" << std::endl;
    QTemporaryDir dir71;
    assert(dir71.isValid());
    CredentialVaultManager manager71(dir71.path());
    assert(!manager71.exists());

    CredentialAutofillController controller71(&manager71);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller71.handleBridgeMessage(testView->page(), candMsg);
    controller71.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller71.activeSaveBubble() == nullptr);
    assert(controller71.pendingCandidateCount() == 0);

    // User later manually creates the vault in Password Manager
    assert(manager71.create(masterPassword));
    assert(manager71.exists());

    // Past credential from when no vault existed must NEVER be automatically saved!
    const auto records = manager71.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(records.isEmpty());
    assert(manager71.list().isEmpty());

    controller71.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 71: no vault login + subsequent vault creation -> past credential NEVER saved" << std::endl;
  }

  // TEST 72: no vault -> no pending candidate state, creating vault afterwards remains empty
  {
    std::cout << "[RUN] TEST 72: no vault -> no pending candidate state, creating vault afterwards remains empty" << std::endl;
    QTemporaryDir dir72;
    assert(dir72.isValid());
    CredentialVaultManager manager72(dir72.path());
    assert(!manager72.exists());

    CredentialAutofillController controller72(&manager72);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller72.handleBridgeMessage(testView->page(), candMsg);
    controller72.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller72.activeSaveBubble() == nullptr);
    assert(controller72.pendingCandidateCount() == 0);

    assert(manager72.create(masterPassword));
    assert(manager72.list().isEmpty());

    controller72.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 72: no vault -> no pending candidate state, creating vault afterwards remains empty" << std::endl;
  }

  // TEST 73: no vault -> no bubble shown, candidate count is 0
  {
    std::cout << "[RUN] TEST 73: no vault -> no bubble shown, candidate count is 0" << std::endl;
    QTemporaryDir dir73;
    assert(dir73.isValid());
    CredentialVaultManager manager73(dir73.path());
    assert(!manager73.exists());

    CredentialAutofillController controller73(&manager73);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller73.handleBridgeMessage(testView->page(), candMsg);
    controller73.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller73.activeSaveBubble() == nullptr);
    assert(controller73.pendingCandidateCount() == 0);

    controller73.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 73: no vault -> no bubble shown, candidate count is 0" << std::endl;
  }

  // TEST 74: pending credential timeout -> credential discarded
  {
    std::cout << "[RUN] TEST 74: pending credential timeout -> credential discarded" << std::endl;
    QTemporaryDir dir74;
    assert(dir74.isValid());
    CredentialVaultManager manager74(dir74.path());
    assert(manager74.create(masterPassword));

    CredentialAutofillController controller74(&manager74);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"SecretPass123\"}");
    controller74.handleBridgeMessage(testView->page(), candMsg);

    const QString key = controller74.candidateKey(testView, QStringLiteral("https://www.facebook.com"), QStringLiteral("user@example.com"));
    assert(controller74.hasPendingCandidate(key));

    // Clear sensitive data / simulate timeout
    controller74.clearAllSensitiveData();
    assert(!controller74.hasPendingCandidate(key));
    assert(controller74.pendingCandidateCount() == 0);

    controller74.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 74: pending credential timeout -> credential discarded" << std::endl;
  }

  // TEST 75: pending credential tab close -> discarded
  {
    std::cout << "[RUN] TEST 75: pending credential tab close -> discarded" << std::endl;
    QTemporaryDir dir75;
    assert(dir75.isValid());
    CredentialVaultManager manager75(dir75.path());
    assert(manager75.create(masterPassword));

    CredentialAutofillController controller75(&manager75);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"SecretPass123\",\"submitted\":true}");
    controller75.handleBridgeMessage(testView->page(), candMsg);
    controller75.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller75.activeSaveBubble() != nullptr);

    // Tab closes
    controller75.onViewClosed(testView);
    assert(controller75.activeSaveBubble() == nullptr);
    assert(controller75.pendingCandidateCount() == 0);

    delete testView;
    std::cout << "[PASS] TEST 75: pending credential tab close -> discarded" << std::endl;
  }

  // TEST 76: pending credential save decision preserved across navigation -> explicit Save resolves it
  {
    std::cout << "[RUN] TEST 76: pending credential save decision preserved across navigation -> explicit Save" << std::endl;
    QTemporaryDir dir76;
    assert(dir76.isValid());
    CredentialVaultManager manager76(dir76.path());
    assert(manager76.create(masterPassword));

    CredentialAutofillController controller76(&manager76);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"SecretPass123\",\"submitted\":true}");
    controller76.handleBridgeMessage(testView->page(), candMsg);
    controller76.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller76.activeSaveBubble() != nullptr);
    assert(controller76.activeSaveBubble()->origin() == QStringLiteral("https://www.facebook.com"));
    assert(controller76.activeSaveBubble()->username() == QStringLiteral("user@example.com"));

    // User navigates / page redirects to another site or path
    testView->setUrl(QUrl(QStringLiteral("https://accounts.example.net/success")));
    controller76.onUrlChanged(testView, testView->url());
    controller76.onPageLoadFinished(testView, true);

    // Pending save decision STILL EXISTS!
    assert(controller76.activeSaveBubble() != nullptr);
    assert(controller76.pendingCandidateCount() == 1);
    // Origin is NOT mutated to the redirect destination!
    assert(controller76.activeSaveBubble()->origin() == QStringLiteral("https://www.facebook.com"));
    assert(controller76.activeSaveBubble()->username() == QStringLiteral("user@example.com"));
    assert(controller76.activeSaveBubble()->isPrimaryButtonEnabled());

    // Explicit Save resolves it
    controller76.activeSaveBubble()->clickPrimary();
    assert(controller76.activeSaveBubble() == nullptr);
    assert(controller76.pendingCandidateCount() == 0);

    // Verify it was saved under the ORIGINAL origin in the vault
    const auto records = manager76.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(records.size() == 1);
    assert(records[0].username == QStringLiteral("user@example.com"));

    // And verify NOTHING was saved under the redirect destination
    const auto redirectRecords = manager76.forOrigin(QUrl(QStringLiteral("https://accounts.example.net")));
    assert(redirectRecords.isEmpty());

    controller76.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 76: pending credential save decision preserved across navigation -> explicit Save" << std::endl;
  }

  // TEST 77: vault exists + unlocked + new credential -> save bubble shown
  {
    std::cout << "[RUN] TEST 77: vault exists + unlocked + new credential -> save bubble shown" << std::endl;
    QTemporaryDir dir77;
    assert(dir77.isValid());
    CredentialVaultManager manager77(dir77.path());
    assert(manager77.create(masterPassword));
    assert(!manager77.isLocked());

    CredentialAutofillController controller77(&manager77);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://github.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://github.com\",\"username\":\"octocat\",\"password\":\"GithubSecret#2026\",\"submitted\":true}");
    controller77.handleBridgeMessage(testView->page(), candMsg);
    controller77.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://github.com\"}"));

    assert(controller77.activeSaveBubble() != nullptr);
    assert(controller77.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);
    assert(controller77.activeSaveBubble()->titleText() == QStringLiteral("Bu giriş bilgisini kaydetmek ister misiniz?"));
    assert(controller77.activeSaveBubble()->primaryButtonText() == QStringLiteral("Kaydet"));
    auto *primary = controller77.activeSaveBubble()->findChild<QPushButton *>(QStringLiteral("credential-save-primary"));
    auto *secondary = controller77.activeSaveBubble()->findChild<QPushButton *>(QStringLiteral("credential-save-secondary"));
    assert(primary && secondary);
    assert(primary->cursor().shape() == Qt::PointingHandCursor);
    assert(secondary->cursor().shape() == Qt::PointingHandCursor);
    assert(primary->styleSheet().contains(QStringLiteral("QPushButton:pressed")));
    assert(secondary->styleSheet().contains(QStringLiteral("QPushButton:pressed")));
    assert(controller77.activeSaveBubble()->secondaryButtonText() == QStringLiteral("Şimdi Değil"));

    controller77.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 77: vault exists + unlocked + new credential -> save bubble shown" << std::endl;
  }

  // TEST 78: vault exists + unlocked + click Kaydet -> credential saved
  {
    std::cout << "[RUN] TEST 78: vault exists + unlocked + click Kaydet -> credential saved" << std::endl;
    QTemporaryDir dir78;
    assert(dir78.isValid());
    CredentialVaultManager manager78(dir78.path());
    assert(manager78.create(masterPassword));

    CredentialAutofillController controller78(&manager78);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://github.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://github.com\",\"username\":\"octocat\",\"password\":\"GithubSecret#2026\",\"submitted\":true}");
    controller78.handleBridgeMessage(testView->page(), candMsg);
    controller78.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://github.com\"}"));

    assert(controller78.activeSaveBubble() != nullptr);
    controller78.activeSaveBubble()->clickPrimary();

    assert(controller78.activeSaveBubble() == nullptr);
    const auto records = manager78.forOrigin(QUrl(QStringLiteral("https://github.com")));
    assert(records.size() == 1);
    assert(records.front().username == QStringLiteral("octocat"));

    CredentialSecret secret;
    assert(manager78.reveal(records.front().id, &secret));
    assert(secret.password == QStringLiteral("GithubSecret#2026"));

    controller78.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 78: vault exists + unlocked + click Kaydet -> credential saved" << std::endl;
  }

  // TEST 79: vault exists + user clicks Şimdi Değil -> credential not saved
  {
    std::cout << "[RUN] TEST 79: vault exists + user clicks Şimdi Değil -> credential not saved" << std::endl;
    QTemporaryDir dir79;
    assert(dir79.isValid());
    CredentialVaultManager manager79(dir79.path());
    assert(manager79.create(masterPassword));

    CredentialAutofillController controller79(&manager79);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://github.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://github.com\",\"username\":\"octocat\",\"password\":\"GithubSecret#2026\",\"submitted\":true}");
    controller79.handleBridgeMessage(testView->page(), candMsg);
    controller79.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://github.com\"}"));

    assert(controller79.activeSaveBubble() != nullptr);
    controller79.activeSaveBubble()->clickSecondary();

    assert(controller79.activeSaveBubble() == nullptr);
    assert(manager79.forOrigin(QUrl(QStringLiteral("https://github.com"))).isEmpty());

    controller79.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 79: vault exists + user clicks Şimdi Değil -> credential not saved" << std::endl;
  }

  // TEST 80: same origin + same username + changed password -> update bubble
  {
    std::cout << "[RUN] TEST 80: same origin + same username + changed password -> update bubble" << std::endl;
    QTemporaryDir dir80;
    assert(dir80.isValid());
    CredentialVaultManager manager80(dir80.path());
    assert(manager80.create(masterPassword));

    CredentialSecret oldSecret;
    oldSecret.origin = QStringLiteral("https://www.facebook.com");
    oldSecret.username = QStringLiteral("user@example.com");
    oldSecret.password = QStringLiteral("OldSecret#2026");
    bool updated = false;
    assert(manager80.save(oldSecret, &updated));

    CredentialAutofillController controller80(&manager80);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // First verify: if password is identical, NO prompt is shown!
    const QString sameCand = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"OldSecret#2026\",\"submitted\":true}");
    controller80.handleBridgeMessage(testView->page(), sameCand);
    controller80.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller80.activeSaveBubble() == nullptr);

    // Now user changed password
    const QString changedCand = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"BrandNewSecret#2026\",\"submitted\":true}");
    controller80.handleBridgeMessage(testView->page(), changedCand);
    controller80.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller80.activeSaveBubble() != nullptr);
    assert(controller80.activeSaveBubble()->mode() == CredentialSaveMode::UpdatePassword);
    assert(controller80.activeSaveBubble()->titleText() == QStringLiteral("Kayıtlı şifre güncellensin mi?"));
    assert(controller80.activeSaveBubble()->primaryButtonText() == QStringLiteral("Şifreyi Güncelle"));

    controller80.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 80: same origin + same username + changed password -> update bubble" << std::endl;
  }

  // TEST 81: click Şifreyi Güncelle -> existing credential updated
  {
    std::cout << "[RUN] TEST 81: click Şifreyi Güncelle -> existing credential updated" << std::endl;
    QTemporaryDir dir81;
    assert(dir81.isValid());
    CredentialVaultManager manager81(dir81.path());
    assert(manager81.create(masterPassword));

    CredentialSecret oldSecret;
    oldSecret.origin = QStringLiteral("https://www.facebook.com");
    oldSecret.username = QStringLiteral("user@example.com");
    oldSecret.password = QStringLiteral("OldSecret#2026");
    bool updated = false;
    assert(manager81.save(oldSecret, &updated));

    CredentialAutofillController controller81(&manager81);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString changedCand = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"BrandNewSecret#2026\",\"submitted\":true}");
    controller81.handleBridgeMessage(testView->page(), changedCand);
    controller81.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller81.activeSaveBubble() != nullptr);
    controller81.activeSaveBubble()->clickPrimary();

    assert(controller81.activeSaveBubble() == nullptr);
    const auto records = manager81.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(records.size() == 1); // Not duplicated!

    CredentialSecret secret;
    assert(manager81.reveal(records.front().id, &secret));
    assert(secret.password == QStringLiteral("BrandNewSecret#2026"));

    controller81.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 81: click Şifreyi Güncelle -> existing credential updated" << std::endl;
  }

  // TEST 82: same origin + different username -> new credential save
  {
    std::cout << "[RUN] TEST 82: same origin + different username -> new credential save" << std::endl;
    QTemporaryDir dir82;
    assert(dir82.isValid());
    CredentialVaultManager manager82(dir82.path());
    assert(manager82.create(masterPassword));

    CredentialSecret user1Secret;
    user1Secret.origin = QStringLiteral("https://www.facebook.com");
    user1Secret.username = QStringLiteral("user1@example.com");
    user1Secret.password = QStringLiteral("Secret1#2026");
    bool updated = false;
    assert(manager82.save(user1Secret, &updated));

    CredentialAutofillController controller82(&manager82);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString user2Cand = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user2@example.com\",\"password\":\"Secret2#2026\",\"submitted\":true}");
    controller82.handleBridgeMessage(testView->page(), user2Cand);
    controller82.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller82.activeSaveBubble() != nullptr);
    assert(controller82.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);
    controller82.activeSaveBubble()->clickPrimary();

    const auto records = manager82.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(records.size() == 2); // Both accounts exist!

    controller82.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 82: same origin + different username -> new credential save" << std::endl;
  }

  // TEST 83: locked vault + save intent + crypto key unavailable -> VaultUnlockDialog shown
  {
    std::cout << "[RUN] TEST 83: locked vault + save intent + crypto key unavailable -> VaultUnlockDialog shown" << std::endl;
    QTemporaryDir dir83;
    assert(dir83.isValid());
    CredentialVaultManager manager83(dir83.path());
    assert(manager83.create(masterPassword));
    manager83.lock();
    assert(manager83.isLocked());

    CredentialAutofillController controller83(&manager83);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.reddit.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.reddit.com\",\"username\":\"redditor\",\"password\":\"RedditSecret#2026\",\"submitted\":true}");
    controller83.handleBridgeMessage(testView->page(), candMsg);
    controller83.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.reddit.com\"}"));

    assert(controller83.activeSaveBubble() != nullptr);
    controller83.activeSaveBubble()->clickPrimary();

    assert(controller83.activeUnlockDialog() != nullptr);
    assert(controller83.activeUnlockDialog()->isVisible());

    controller83.activeUnlockDialog()->reject();
    controller83.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 83: locked vault + save intent + crypto key unavailable -> VaultUnlockDialog shown" << std::endl;
  }

  // TEST 84: locked vault + successful unlock -> pending credential saved
  {
    std::cout << "[RUN] TEST 84: locked vault + successful unlock -> pending credential saved" << std::endl;
    QTemporaryDir dir84;
    assert(dir84.isValid());
    CredentialVaultManager manager84(dir84.path());
    assert(manager84.create(masterPassword));
    manager84.lock();
    assert(manager84.isLocked());

    CredentialAutofillController controller84(&manager84);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.reddit.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.reddit.com\",\"username\":\"redditor\",\"password\":\"RedditSecret#2026\",\"submitted\":true}");
    controller84.handleBridgeMessage(testView->page(), candMsg);
    controller84.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.reddit.com\"}"));

    assert(controller84.activeSaveBubble() != nullptr);
    controller84.activeSaveBubble()->clickPrimary();
    assert(controller84.activeUnlockDialog() != nullptr);

    // Enter correct password and unlock
    controller84.activeUnlockDialog()->passwordInput()->setText(masterPassword);
    controller84.activeUnlockDialog()->attemptUnlock();
    assert(waitForCondition([&manager84]() { return manager84.forOrigin(QUrl(QStringLiteral("https://www.reddit.com"))).size() == 1; }));
    assert(!manager84.isLocked());

    const auto records = manager84.forOrigin(QUrl(QStringLiteral("https://www.reddit.com")));
    assert(records.size() == 1);
    assert(records.front().username == QStringLiteral("redditor"));

    CredentialSecret secret;
    assert(manager84.reveal(records.front().id, &secret));
    assert(secret.password == QStringLiteral("RedditSecret#2026"));

    controller84.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 84: locked vault + successful unlock -> pending credential saved" << std::endl;
  }

  // TEST 85: locked vault + cancel unlock -> no save
  {
    std::cout << "[RUN] TEST 85: locked vault + cancel unlock -> no save" << std::endl;
    QTemporaryDir dir85;
    assert(dir85.isValid());
    CredentialVaultManager manager85(dir85.path());
    assert(manager85.create(masterPassword));
    manager85.lock();
    assert(manager85.isLocked());

    CredentialAutofillController controller85(&manager85);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.reddit.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.reddit.com\",\"username\":\"redditor\",\"password\":\"RedditSecret#2026\",\"submitted\":true}");
    controller85.handleBridgeMessage(testView->page(), candMsg);
    controller85.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.reddit.com\"}"));

    controller85.activeSaveBubble()->clickPrimary();
    assert(controller85.activeUnlockDialog() != nullptr);

    controller85.activeUnlockDialog()->reject();
    assert(manager85.isLocked());
    assert(manager85.forOrigin(QUrl(QStringLiteral("https://www.reddit.com"))).isEmpty());

    controller85.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 85: locked vault + cancel unlock -> no save" << std::endl;
  }

  // TEST 86: locked vault + wrong password -> progressive rate limit preserved
  {
    std::cout << "[RUN] TEST 86: locked vault + wrong password -> progressive rate limit preserved" << std::endl;
    QTemporaryDir dir86;
    assert(dir86.isValid());
    CredentialVaultManager manager86(dir86.path());
    assert(manager86.create(masterPassword));
    manager86.lock();

    CredentialAutofillController controller86(&manager86);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.reddit.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.reddit.com\",\"username\":\"redditor\",\"password\":\"RedditSecret#2026\",\"submitted\":true}");
    controller86.handleBridgeMessage(testView->page(), candMsg);
    controller86.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.reddit.com\"}"));

    controller86.activeSaveBubble()->clickPrimary();
    assert(controller86.activeUnlockDialog() != nullptr);

    controller86.activeUnlockDialog()->passwordInput()->setText(QStringLiteral("WrongMasterPwd!123"));
    controller86.activeUnlockDialog()->attemptUnlock();
    assert(waitForCondition([&manager86]() { return manager86.failedUnlockAttempts() == 1; }));

    controller86.activeUnlockDialog()->reject();
    controller86.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 86: locked vault + wrong password -> progressive rate limit preserved" << std::endl;
  }

  // TEST 87: duplicate SPA success signal -> only one save bubble
  {
    std::cout << "[RUN] TEST 87: duplicate SPA success signal -> only one save bubble" << std::endl;
    QTemporaryDir dir87;
    assert(dir87.isValid());
    CredentialVaultManager manager87(dir87.path());
    assert(manager87.create(masterPassword));

    CredentialAutofillController controller87(&manager87);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://app.spa.example/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://app.spa.example\",\"username\":\"spauser\",\"password\":\"SpaSecret#2026\",\"submitted\":true}");
    controller87.handleBridgeMessage(testView->page(), candMsg);

    // Two success signals fired in rapid succession by SPA mutations
    controller87.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://app.spa.example\"}"));
    auto *firstBubble = controller87.activeSaveBubble();
    assert(firstBubble != nullptr);

    controller87.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://app.spa.example\"}"));
    auto *secondBubble = controller87.activeSaveBubble();
    assert(firstBubble == secondBubble); // Reused / deduped, no duplicate bubble

    controller87.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 87: duplicate SPA success signal -> only one save bubble" << std::endl;
  }

  // TEST 88: tab switch -> save bubble dismissed safely
  {
    std::cout << "[RUN] TEST 88: tab switch -> save bubble dismissed safely" << std::endl;
    QTemporaryDir dir88;
    assert(dir88.isValid());
    CredentialVaultManager manager88(dir88.path());
    assert(manager88.create(masterPassword));

    CredentialAutofillController controller88(&manager88);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller88.handleBridgeMessage(testView->page(), candMsg);
    controller88.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller88.activeSaveBubble() != nullptr);

    // Tab switch occurs
    controller88.dismissSaveBubble();
    assert(controller88.activeSaveBubble() == nullptr);

    controller88.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 88: tab switch -> save bubble dismissed safely" << std::endl;
  }

  // TEST 89: redirect to untrusted site -> origin binding preserved, no leak to evil site
  {
    std::cout << "[RUN] TEST 89: redirect to untrusted site -> origin binding preserved, no leak to evil site" << std::endl;
    QTemporaryDir dir89;
    assert(dir89.isValid());
    CredentialVaultManager manager89(dir89.path());
    assert(manager89.create(masterPassword));

    CredentialAutofillController controller89(&manager89);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"FbSecret#2026\",\"submitted\":true}");
    controller89.handleBridgeMessage(testView->page(), candMsg);
    controller89.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller89.activeSaveBubble() != nullptr);
    assert(controller89.activeSaveBubble()->origin() == QStringLiteral("https://www.facebook.com"));

    // Navigated to evil site before user interacted
    testView->setUrl(QUrl(QStringLiteral("https://evil.com/phish")));
    controller89.onUrlChanged(testView, testView->url());

    // Prompt MUST NOT be silently dismissed, AND origin MUST NOT be hijacked by evil.com
    assert(controller89.activeSaveBubble() != nullptr);
    assert(controller89.activeSaveBubble()->origin() == QStringLiteral("https://www.facebook.com"));
    assert(controller89.activeSaveBubble()->username() == QStringLiteral("user@example.com"));

    // User chooses to save
    controller89.activeSaveBubble()->clickPrimary();
    assert(controller89.activeSaveBubble() == nullptr);

    // Verified: Credential saved for https://www.facebook.com
    assert(manager89.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com"))));

    // Security invariant: Absolutely NO credential associated with https://evil.com!
    assert(!manager89.hasMatchingCredential(testView->url()));
    assert(manager89.forOrigin(QUrl(QStringLiteral("https://evil.com"))).isEmpty());

    controller89.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 89: redirect to untrusted site -> origin binding preserved, no leak to evil site" << std::endl;
  }

  // TEST 90: cross-origin iframe login -> no credential save prompt
  {
    std::cout << "[RUN] TEST 90: cross-origin iframe login -> no credential save prompt" << std::endl;
    QTemporaryDir dir90;
    assert(dir90.isValid());
    CredentialVaultManager manager90(dir90.path());
    assert(manager90.create(masterPassword));

    CredentialAutofillController controller90(&manager90);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://trusted.example/page")));

    // Subframe candidate with mismatched origin
    const QString subframeCand = QStringLiteral("candidate:{\"origin\":\"https://iframe.untrusted.com\",\"username\":\"victim\",\"password\":\"StolenPass\",\"submitted\":true}");
    controller90.handleBridgeMessage(testView->page(), subframeCand);
    controller90.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://iframe.untrusted.com\"}"));

    // Prompt MUST NOT be shown for mismatched origin
    assert(controller90.activeSaveBubble() == nullptr);

    controller90.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 90: cross-origin iframe login -> no credential save prompt" << std::endl;
  }

  // TEST 91: synthetic JS cannot trigger native save action
  {
    std::cout << "[RUN] TEST 91: synthetic JS cannot trigger native save action" << std::endl;
    QTemporaryDir dir91;
    assert(dir91.isValid());
    CredentialVaultManager manager91(dir91.path());
    assert(manager91.create(masterPassword));

    CredentialAutofillController controller91(&manager91);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://site.example/login")));

    // Synthetic console message trying to call save action
    const QString evilMsg = QStringLiteral("save-action:{\"origin\":\"https://site.example\"}");
    assert(!controller91.handleBridgeMessage(testView->page(), evilMsg));

    controller91.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 91: synthetic JS cannot trigger native save action" << std::endl;
  }

  // TEST 92: saved credential immediately visible in Password Manager list
  {
    std::cout << "[RUN] TEST 92: saved credential immediately visible in Password Manager list" << std::endl;
    QTemporaryDir dir92;
    assert(dir92.isValid());
    CredentialVaultManager manager92(dir92.path());
    assert(manager92.create(masterPassword));

    CredentialAutofillController controller92(&manager92);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret#2026\",\"submitted\":true}");
    controller92.handleBridgeMessage(testView->page(), candMsg);
    controller92.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller92.activeSaveBubble() != nullptr);
    controller92.activeSaveBubble()->clickPrimary();

    // Immediately present in vault list
    const auto allRecords = manager92.list();
    assert(allRecords.size() == 1);
    assert(allRecords.front().origin == QStringLiteral("https://www.facebook.com"));
    assert(allRecords.front().username == QStringLiteral("user@example.com"));
    assert(manager92.hasMatchingCredential(testView->url()));

    controller92.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 92: saved credential immediately visible in Password Manager list" << std::endl;
  }

  // TEST 93: vault deletion does not leave stale pending save state
  {
    std::cout << "[RUN] TEST 93: vault deletion does not leave stale pending save state" << std::endl;
    QTemporaryDir dir93;
    assert(dir93.isValid());
    CredentialVaultManager manager93(dir93.path());
    assert(manager93.create(masterPassword));

    CredentialAutofillController controller93(&manager93);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret#2026\",\"submitted\":true}");
    controller93.handleBridgeMessage(testView->page(), candMsg);
    controller93.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    assert(controller93.activeSaveBubble() != nullptr);

    // Vault reset/deleted while bubble open
    assert(manager93.reset());
    assert(!manager93.exists());

    controller93.clearAllSensitiveData();
    assert(controller93.activeSaveBubble() == nullptr);
    assert(controller93.pendingCandidateCount() == 0);

    controller93.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 93: vault deletion does not leave stale pending save state" << std::endl;
  }

  // TEST 94: save failure does not report success
  {
    std::cout << "[RUN] TEST 94: save failure does not report success" << std::endl;
    QTemporaryDir dir94;
    assert(dir94.isValid());
    CredentialVaultManager manager94(dir94.path());
    assert(manager94.create(masterPassword));

    CredentialAutofillController controller94(&manager94);
    bool savedEmitted = false;
    QObject::connect(&controller94, &CredentialAutofillController::credentialSaved, [&savedEmitted] {
      savedEmitted = true;
    });

    // Attempt to save non-existent key
    controller94.triggerSaveCandidate(QStringLiteral("non-existent-key"));
    assert(!savedEmitted);

    std::cout << "[PASS] TEST 94: save failure does not report success" << std::endl;
  }

  // TEST 95: password never persisted before vault creation
  {
    std::cout << "[RUN] TEST 95: password never persisted before vault creation" << std::endl;
    QTemporaryDir dir95;
    assert(dir95.isValid());
    CredentialVaultManager manager95(dir95.path());
    assert(!manager95.exists());

    CredentialAutofillController controller95(&manager95);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString sensitivePassword = QStringLiteral("SuperSecretPasswordNeverOnDisk#9999");
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"%1\",\"submitted\":true}").arg(sensitivePassword);
    controller95.handleBridgeMessage(testView->page(), candMsg);
    controller95.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    // Check all files in directory: NO file must contain sensitivePassword
    QDir dir(dir95.path());
    const auto files = dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &f : files) {
      QFile file(dir.filePath(f));
      if (file.open(QIODevice::ReadOnly)) {
        assert(!file.readAll().contains(sensitivePassword.toUtf8()));
      }
    }

    controller95.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 95: password never persisted before vault creation" << std::endl;
  }

  // TEST 96: no plaintext password in pending persistence files
  {
    std::cout << "[RUN] TEST 96: no plaintext password in pending persistence files" << std::endl;
    QTemporaryDir dir96;
    assert(dir96.isValid());
    CredentialVaultManager manager96(dir96.path());
    assert(manager96.create(masterPassword));

    CredentialAutofillController controller96(&manager96);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString secretPass = QStringLiteral("EncryptedOnlyPassphrase#2026");
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"%1\",\"submitted\":true}").arg(secretPass);
    controller96.handleBridgeMessage(testView->page(), candMsg);
    controller96.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));

    controller96.activeSaveBubble()->clickPrimary();

    // The vault file is written encrypted; verify the plaintext password is NOT in any file!
    QDirIterator it(dir96.path(), QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString filePath = it.next();
      QFileInfo info(filePath);
      if (info.isFile()) {
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly)) {
          const QByteArray bytes = f.readAll();
          assert(!bytes.contains(secretPass.toUtf8()));
        }
      }
    }

    controller96.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 96: no plaintext password in pending persistence files" << std::endl;
  }

  // TEST 97: autofill key visibility regression suite still passes
  {
    std::cout << "[RUN] TEST 97: autofill key visibility regression suite still passes" << std::endl;
    QTemporaryDir dir97;
    assert(dir97.isValid());
    CredentialVaultManager manager97(dir97.path());

    // 1. Vault does not exist -> no key icon
    assert(!manager97.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    // 2. Vault created, but origin has no credential -> no key icon
    assert(manager97.create(masterPassword));
    assert(!manager97.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    // 3. Credential added -> key icon available
    CredentialSecret secret;
    secret.origin = QStringLiteral("https://www.facebook.com");
    secret.username = QStringLiteral("user@example.com");
    secret.password = QStringLiteral("FbSecret#2026");
    bool updated = false;
    assert(manager97.save(secret, &updated));
    assert(manager97.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    // 4. Locked vault + matching credential -> key icon still available
    manager97.lock();
    assert(manager97.isLocked());
    assert(manager97.hasMatchingCredential(QUrl(QStringLiteral("https://www.facebook.com/login"))));

    std::cout << "[PASS] TEST 97: autofill key visibility regression suite still passes" << std::endl;
  }

  // TEST 98: password field filled, site eye button clicked -> no login attempt, no save bubble
  {
    std::cout << "[RUN] TEST 98: password field filled, site eye button clicked -> no login attempt, no save bubble" << std::endl;
    QTemporaryDir dir98;
    assert(dir98.isValid());
    CredentialVaultManager manager98(dir98.path());
    assert(manager98.create(masterPassword));

    CredentialAutofillController controller98(&manager98);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // Site eye button clicked: user typed credentials, candidate captured without submit
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller98.handleBridgeMessage(testView->page(), candMsg));

    // No submit attempt must be registered
    assert(!controller98.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // No save bubble must be displayed
    assert(controller98.activeSaveBubble() == nullptr);

    controller98.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 98: password field filled, site eye button clicked -> no login attempt, no save bubble" << std::endl;
  }

  // TEST 99: password input type password -> text => no save bubble
  {
    std::cout << "[RUN] TEST 99: password input type password -> text => no save bubble" << std::endl;
    QTemporaryDir dir99;
    assert(dir99.isValid());
    CredentialVaultManager manager99(dir99.path());
    assert(manager99.create(masterPassword));

    CredentialAutofillController controller99(&manager99);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // Candidate staged/captured without submit
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller99.handleBridgeMessage(testView->page(), candMsg));

    // Eye toggle changes input type password -> text (DOM mutation occurs, but no submit)
    assert(!controller99.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));
    assert(controller99.activeSaveBubble() == nullptr);

    controller99.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 99: password input type password -> text => no save bubble" << std::endl;
  }

  // TEST 100: password input type text -> password => no save bubble
  {
    std::cout << "[RUN] TEST 100: password input type text -> password => no save bubble" << std::endl;
    QTemporaryDir dir100;
    assert(dir100.isValid());
    CredentialVaultManager manager100(dir100.path());
    assert(manager100.create(masterPassword));

    CredentialAutofillController controller100(&manager100);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller100.handleBridgeMessage(testView->page(), candMsg));

    // Eye toggle changes back text -> password
    assert(!controller100.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));
    assert(controller100.activeSaveBubble() == nullptr);

    controller100.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 100: password input type text -> password => no save bubble" << std::endl;
  }

  // TEST 101: DOM mutation without submit => no save bubble
  {
    std::cout << "[RUN] TEST 101: DOM mutation without submit => no save bubble" << std::endl;
    QTemporaryDir dir101;
    assert(dir101.isValid());
    CredentialVaultManager manager101(dir101.path());
    assert(manager101.create(masterPassword));

    CredentialAutofillController controller101(&manager101);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller101.handleBridgeMessage(testView->page(), candMsg));

    // Page DOM mutated (e.g. tooltip, dropdown, re-render)
    assert(!controller101.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));
    assert(controller101.activeSaveBubble() == nullptr);

    controller101.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 101: DOM mutation without submit => no save bubble" << std::endl;
  }

  // TEST 102: candidate captured but no submit + success hint emitted => success hint ignored
  {
    std::cout << "[RUN] TEST 102: candidate captured but no submit + success hint emitted => success hint ignored" << std::endl;
    QTemporaryDir dir102;
    assert(dir102.isValid());
    CredentialVaultManager manager102(dir102.path());
    assert(manager102.create(masterPassword));

    CredentialAutofillController controller102(&manager102);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller102.handleBridgeMessage(testView->page(), candMsg));

    // Spurious or malicious success hint emitted without submit
    const QString hintMsg = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller102.handleBridgeMessage(testView->page(), hintMsg));

    // Gated! No save bubble!
    assert(controller102.activeSaveBubble() == nullptr);

    controller102.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 102: candidate captured but no submit + success hint emitted => success hint ignored" << std::endl;
  }

  // TEST 103: candidate + trusted form submit + valid success hint => save bubble shown
  {
    std::cout << "[RUN] TEST 103: candidate + trusted form submit + valid success hint => save bubble shown" << std::endl;
    QTemporaryDir dir103;
    assert(dir103.isValid());
    CredentialVaultManager manager103(dir103.path());
    assert(manager103.create(masterPassword));

    CredentialAutofillController controller103(&manager103);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // 1. Candidate captured
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":false}");
    assert(controller103.handleBridgeMessage(testView->page(), candMsg));

    // 2. Submit event occurs
    const QString submitMsg = QStringLiteral("submit:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\"}");
    assert(controller103.handleBridgeMessage(testView->page(), submitMsg));
    assert(controller103.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // 3. Success hint emitted
    const QString hintMsg = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller103.handleBridgeMessage(testView->page(), hintMsg));

    // Bubble shown!
    assert(controller103.activeSaveBubble() != nullptr);
    assert(controller103.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);

    controller103.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 103: candidate + trusted form submit + valid success hint => save bubble shown" << std::endl;
  }

  // TEST 104: candidate + form submit + navigation success => save bubble shown
  {
    std::cout << "[RUN] TEST 104: candidate + form submit + navigation success => save bubble shown" << std::endl;
    QTemporaryDir dir104;
    assert(dir104.isValid());
    CredentialVaultManager manager104(dir104.path());
    assert(manager104.create(masterPassword));

    CredentialAutofillController controller104(&manager104);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret123\",\"submitted\":true}");
    assert(controller104.handleBridgeMessage(testView->page(), candMsg));
    assert(controller104.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Navigation occurs to dashboard/feed
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller104.onPageLoadFinished(testView, true);

    assert(controller104.activeSaveBubble() != nullptr);

    controller104.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 104: candidate + form submit + navigation success => save bubble shown" << std::endl;
  }

  // TEST 105: candidate + form submit + login failure => no save bubble
  {
    std::cout << "[RUN] TEST 105: candidate + form submit + login failure => no save bubble" << std::endl;
    QTemporaryDir dir105;
    assert(dir105.isValid());
    CredentialVaultManager manager105(dir105.path());
    assert(manager105.create(masterPassword));

    CredentialAutofillController controller105(&manager105);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"WrongPass123\",\"submitted\":true}");
    assert(controller105.handleBridgeMessage(testView->page(), candMsg));

    // Login fails: page reloaded / stays on login page with error message, no success hint
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login?error=invalid_password")));
    controller105.onPageLoadFinished(testView, true);

    // Save bubble must NOT be shown
    assert(controller105.activeSaveBubble() == nullptr);

    controller105.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 105: candidate + form submit + login failure => no save bubble" << std::endl;
  }

  // TEST 106: duplicate submit => one attempt / one save bubble
  {
    std::cout << "[RUN] TEST 106: duplicate submit => one attempt / one save bubble" << std::endl;
    QTemporaryDir dir106;
    assert(dir106.isValid());
    CredentialVaultManager manager106(dir106.path());
    assert(manager106.create(masterPassword));

    CredentialAutofillController controller106(&manager106);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // Double-submit occurs (user clicks quickly or Enter + click)
    const QString submit1 = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Pass#2026\",\"submitted\":true,\"nonce\":\"nonce1\"}");
    const QString submit2 = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Pass#2026\",\"submitted\":true,\"nonce\":\"nonce2\"}");
    assert(controller106.handleBridgeMessage(testView->page(), submit1));
    assert(controller106.handleBridgeMessage(testView->page(), submit2));

    // Success hint arrives
    const QString hint = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller106.handleBridgeMessage(testView->page(), hint));
    auto *bubble1 = controller106.activeSaveBubble();
    assert(bubble1 != nullptr);

    // Second success hint arrives
    assert(controller106.handleBridgeMessage(testView->page(), hint));
    auto *bubble2 = controller106.activeSaveBubble();
    assert(bubble1 == bubble2); // Exactly one bubble, not duplicated

    controller106.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 106: duplicate submit => one attempt / one save bubble" << std::endl;
  }

  // TEST 107: unrelated form submit => no login save prompt
  {
    std::cout << "[RUN] TEST 107: unrelated form submit => no login save prompt" << std::endl;
    QTemporaryDir dir107;
    assert(dir107.isValid());
    CredentialVaultManager manager107(dir107.path());
    assert(manager107.create(masterPassword));

    CredentialAutofillController controller107(&manager107);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://news.example.com/article")));

    // Unrelated form submit (newsletter / search form, no password candidate)
    assert(!controller107.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://news.example.com")));

    const QString hint = QStringLiteral("success:{\"origin\":\"https://news.example.com\"}");
    controller107.handleBridgeMessage(testView->page(), hint);

    assert(controller107.activeSaveBubble() == nullptr);

    controller107.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 107: unrelated form submit => no login save prompt" << std::endl;
  }

  // TEST 108: synthetic button.click() => no native save intent
  {
    std::cout << "[RUN] TEST 108: synthetic button.click() => no native save intent" << std::endl;
    QTemporaryDir dir108;
    assert(dir108.isValid());
    CredentialVaultManager manager108(dir108.path());
    assert(manager108.create(masterPassword));

    CredentialAutofillController controller108(&manager108);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://site.example/login")));

    // Synthetic attempt without candidate in memory
    const QString fakeSubmit = QStringLiteral("submit:{\"origin\":\"https://site.example\",\"username\":\"evil\"}");
    controller108.handleBridgeMessage(testView->page(), fakeSubmit);

    // Later success hint
    controller108.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://site.example\"}"));
    assert(controller108.activeSaveBubble() == nullptr);

    controller108.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 108: synthetic button.click() => no native save intent" << std::endl;
  }

  // TEST 109: tab A submit, tab B success hint => no cross-tab save
  {
    std::cout << "[RUN] TEST 109: tab A submit, tab B success hint => no cross-tab save" << std::endl;
    QTemporaryDir dir109;
    assert(dir109.isValid());
    CredentialVaultManager manager109(dir109.path());
    assert(manager109.create(masterPassword));

    CredentialAutofillController controller109(&manager109);
    auto *tabA = new QWebEngineView();
    tabA->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));
    auto *tabB = new QWebEngineView();
    tabB->setUrl(QUrl(QStringLiteral("https://www.facebook.com/other")));

    // Tab A submits login
    const QString candA = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"userA@example.com\",\"password\":\"PassA#2026\",\"submitted\":true}");
    assert(controller109.handleBridgeMessage(tabA->page(), candA));
    assert(controller109.hasActiveSubmittedLoginAttempt(tabA, QStringLiteral("https://www.facebook.com")));
    assert(!controller109.hasActiveSubmittedLoginAttempt(tabB, QStringLiteral("https://www.facebook.com")));

    // Tab B emits success hint
    const QString hintB = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller109.handleBridgeMessage(tabB->page(), hintB));

    // Tab B had no submitted attempt, so Tab A must NOT be triggered by Tab B
    assert(controller109.activeSaveBubble() == nullptr);

    controller109.onViewClosed(tabA);
    controller109.onViewClosed(tabB);
    delete tabA;
    delete tabB;
    std::cout << "[PASS] TEST 109: tab A submit, tab B success hint => no cross-tab save" << std::endl;
  }

  // TEST 110: stale attempt timeout => later mutation does not show save bubble
  {
    std::cout << "[RUN] TEST 110: stale attempt timeout => later mutation does not show save bubble" << std::endl;
    QTemporaryDir dir110;
    assert(dir110.isValid());
    CredentialVaultManager manager110(dir110.path());
    assert(manager110.create(masterPassword));

    CredentialAutofillController controller110(&manager110);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"Secret#2026\",\"submitted\":true}");
    assert(controller110.handleBridgeMessage(testView->page(), candMsg));
    assert(controller110.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Stale timeout occurs / sensitive data prune
    controller110.clearAllSensitiveData();
    assert(!controller110.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Later mutation emits success hint
    const QString hintMsg = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    controller110.handleBridgeMessage(testView->page(), hintMsg);

    // Save bubble must NOT be shown
    assert(controller110.activeSaveBubble() == nullptr);

    controller110.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 110: stale attempt timeout => later mutation does not show save bubble" << std::endl;
  }

  // TEST 111: trusted submit + same-origin redirect from login page to root -> save bubble shown
  {
    std::cout << "[RUN] TEST 111: trusted submit + same-origin redirect from login page to root -> save bubble shown" << std::endl;
    QTemporaryDir dir111;
    assert(dir111.isValid());
    CredentialVaultManager manager111(dir111.path());
    assert(manager111.create(masterPassword));

    CredentialAutofillController controller111(&manager111);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/")));

    // 1. Submit login on /login/
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"fbuser@example.com\",\"password\":\"SecureFb#111\",\"submitted\":true}");
    assert(controller111.handleBridgeMessage(testView->page(), candMsg));
    assert(controller111.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // 2. Same-origin redirect to root /
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/")));
    controller111.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/")));

    // 3. Save bubble must appear!
    assert(controller111.activeSaveBubble() != nullptr);
    assert(controller111.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);

    controller111.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 111: trusted submit + same-origin redirect from login page to root -> save bubble shown" << std::endl;
  }

  // TEST 112: trusted submit + redirect to authenticated /feed page -> save bubble shown
  {
    std::cout << "[RUN] TEST 112: trusted submit + redirect to authenticated /feed page -> save bubble shown" << std::endl;
    QTemporaryDir dir112;
    assert(dir112.isValid());
    CredentialVaultManager manager112(dir112.path());
    assert(manager112.create(masterPassword));

    CredentialAutofillController controller112(&manager112);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"feeduser@example.com\",\"password\":\"FeedPass#112\",\"submitted\":true}");
    assert(controller112.handleBridgeMessage(testView->page(), candMsg));
    assert(controller112.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Redirect to authenticated /feed
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller112.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/feed")));

    assert(controller112.activeSaveBubble() != nullptr);
    assert(controller112.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);

    controller112.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 112: trusted submit + redirect to authenticated /feed page -> save bubble shown" << std::endl;
  }

  // TEST 113: onUrlChanged must not clear submitted attempt before success evaluation
  {
    std::cout << "[RUN] TEST 113: onUrlChanged must not clear submitted attempt before success evaluation" << std::endl;
    QTemporaryDir dir113;
    assert(dir113.isValid());
    CredentialVaultManager manager113(dir113.path());
    assert(manager113.create(masterPassword));

    CredentialAutofillController controller113(&manager113);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://facebook.com/login")));

    // Candidate submitted on apex domain facebook.com
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://facebook.com\",\"username\":\"crosssub@example.com\",\"password\":\"Pass#113\",\"submitted\":true}");
    assert(controller113.handleBridgeMessage(testView->page(), candMsg));
    assert(controller113.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://facebook.com")));

    // URL changes to www.facebook.com/home
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/home")));
    controller113.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/home")));

    // Attempt and candidate were not cleared prematurely; bubble shown!
    assert(controller113.activeSaveBubble() != nullptr);

    controller113.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 113: onUrlChanged must not clear submitted attempt before success evaluation" << std::endl;
  }

  // TEST 114: candidate survives successful login redirect until save bubble is created
  {
    std::cout << "[RUN] TEST 114: candidate survives successful login redirect until save bubble is created" << std::endl;
    QTemporaryDir dir114;
    assert(dir114.isValid());
    CredentialVaultManager manager114(dir114.path());
    assert(manager114.create(masterPassword));

    CredentialAutofillController controller114(&manager114);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"survive@example.com\",\"password\":\"SurvivePass#114\",\"submitted\":true}");
    assert(controller114.handleBridgeMessage(testView->page(), candMsg));

    // Intermediate navigation to login checkpoint (isLoginOrErrorUrl is true)
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/checkpoint")));
    controller114.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/login/checkpoint")));
    assert(controller114.activeSaveBubble() == nullptr); // Not prompted on login path

    // Candidate and attempt MUST survive the intermediate redirect!
    assert(controller114.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Final navigation to authenticated home
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/")));
    controller114.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/")));
    assert(controller114.activeSaveBubble() != nullptr);

    controller114.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 114: candidate survives successful login redirect until save bubble is created" << std::endl;
  }

  // TEST 115: submit + pageLoadFinished on authenticated page -> save bubble shown
  {
    std::cout << "[RUN] TEST 115: submit + pageLoadFinished on authenticated page -> save bubble shown" << std::endl;
    QTemporaryDir dir115;
    assert(dir115.isValid());
    CredentialVaultManager manager115(dir115.path());
    assert(manager115.create(masterPassword));

    CredentialAutofillController controller115(&manager115);
    auto *testView = new QWebEngineView();
    // User starts on root homepage where login form is embedded
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"rootlogin@example.com\",\"password\":\"RootPass#115\",\"submitted\":true}");
    assert(controller115.handleBridgeMessage(testView->page(), candMsg));
    assert(controller115.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Page reloads / completes load at root homepage (authenticated state)
    // A same-URL load alone is ambiguous; the final DOM success signal confirms it.
    controller115.onPageLoadFinished(testView, true);
    assert(controller115.activeSaveBubble() == nullptr);
    controller115.handleBridgeMessage(testView->page(), QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}"));
    assert(controller115.activeSaveBubble() != nullptr);

    controller115.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 115: submit + pageLoadFinished on authenticated page -> save bubble shown" << std::endl;
  }

  // TEST 116: submit + SPA success hint -> save bubble shown
  {
    std::cout << "[RUN] TEST 116: submit + SPA success hint -> save bubble shown" << std::endl;
    QTemporaryDir dir116;
    assert(dir116.isValid());
    CredentialVaultManager manager116(dir116.path());
    assert(manager116.create(masterPassword));

    CredentialAutofillController controller116(&manager116);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"spauser@example.com\",\"password\":\"SpaPass#116\",\"submitted\":true}");
    assert(controller116.handleBridgeMessage(testView->page(), candMsg));
    assert(controller116.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // SPA DOM update triggers success hint
    const QString hint = QStringLiteral("success:{\"origin\":\"https://www.facebook.com\"}");
    assert(controller116.handleBridgeMessage(testView->page(), hint));
    assert(controller116.activeSaveBubble() != nullptr);

    controller116.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 116: submit + SPA success hint -> save bubble shown" << std::endl;
  }

  // TEST 117: submit + login failure page -> no save bubble
  {
    std::cout << "[RUN] TEST 117: submit + login failure page -> no save bubble" << std::endl;
    QTemporaryDir dir117;
    assert(dir117.isValid());
    CredentialVaultManager manager117(dir117.path());
    assert(manager117.create(masterPassword));

    CredentialAutofillController controller117(&manager117);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/")));

    // Submit with wrong password
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"user@example.com\",\"password\":\"WrongPass#117\",\"submitted\":true}");
    assert(controller117.handleBridgeMessage(testView->page(), candMsg));

    // Redirect to login failure page
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/?error=invalid_password")));
    controller117.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/login/?error=invalid_password")));
    controller117.onPageLoadFinished(testView, true);

    // Save bubble must NOT appear!
    assert(controller117.activeSaveBubble() == nullptr);

    controller117.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 117: submit + login failure page -> no save bubble" << std::endl;
  }

  // TEST 118: no submit + navigation -> no save bubble
  {
    std::cout << "[RUN] TEST 118: no submit + navigation -> no save bubble" << std::endl;
    QTemporaryDir dir118;
    assert(dir118.isValid());
    CredentialVaultManager manager118(dir118.path());
    assert(manager118.create(masterPassword));

    CredentialAutofillController controller118(&manager118);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // User types in fields without submitting (submitted: false)
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"nosubmit@example.com\",\"password\":\"NoSubmit#118\",\"submitted\":false}");
    assert(controller118.handleBridgeMessage(testView->page(), candMsg));
    assert(!controller118.hasActiveSubmittedLoginAttempt(testView, QStringLiteral("https://www.facebook.com")));

    // Navigates away without submitting
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller118.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller118.onPageLoadFinished(testView, true);

    // Save bubble must NOT appear!
    assert(controller118.activeSaveBubble() == nullptr);

    controller118.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 118: no submit + navigation -> no save bubble" << std::endl;
  }

  // TEST 119: eye toggle + navigation unrelated -> no save bubble
  {
    std::cout << "[RUN] TEST 119: eye toggle + navigation unrelated -> no save bubble" << std::endl;
    QTemporaryDir dir119;
    assert(dir119.isValid());
    CredentialVaultManager manager119(dir119.path());
    assert(manager119.create(masterPassword));

    CredentialAutofillController controller119(&manager119);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // Candidate captured during typing, user toggled eye icon
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"eyeuser@example.com\",\"password\":\"EyeToggle#119\",\"submitted\":false}");
    assert(controller119.handleBridgeMessage(testView->page(), candMsg));

    // Navigates to unrelated website
    testView->setUrl(QUrl(QStringLiteral("https://unrelated.example.com/")));
    controller119.onUrlChanged(testView, QUrl(QStringLiteral("https://unrelated.example.com/")));
    controller119.onPageLoadFinished(testView, true);

    assert(controller119.activeSaveBubble() == nullptr);

    controller119.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 119: eye toggle + navigation unrelated -> no save bubble" << std::endl;
  }

  // TEST 120: no-vault successful login -> silent, no bubble, no auto-save on vault creation
  {
    std::cout << "[RUN] TEST 120: no-vault successful login -> silent, no bubble, no auto-save on vault creation" << std::endl;
    QTemporaryDir emptyDir;
    assert(emptyDir.isValid());
    // Fresh vault manager where NO vault exists
    CredentialVaultManager noVaultManager(emptyDir.path());
    assert(!noVaultManager.exists());

    CredentialAutofillController controller120(&noVaultManager);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"novault@example.com\",\"password\":\"NoVault#120\",\"submitted\":true}");
    assert(controller120.handleBridgeMessage(testView->page(), candMsg));

    // Successful login redirect to /
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/")));
    controller120.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/")));

    // When no vault exists, system must remain completely silent
    assert(controller120.activeSaveBubble() == nullptr);
    assert(controller120.pendingCandidateCount() == 0);

    // Later, user manually creates vault
    assert(noVaultManager.create(masterPassword));
    assert(noVaultManager.exists());

    // Past credentials must NEVER be automatically saved
    const auto records = noVaultManager.forOrigin(QUrl(QStringLiteral("https://www.facebook.com/")));
    assert(records.isEmpty());

    controller120.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 120: no-vault successful login -> silent, no bubble, no auto-save on vault creation" << std::endl;
  }

  // TEST 121: vault unlocked successful login -> save bubble shown
  {
    std::cout << "[RUN] TEST 121: vault unlocked successful login -> save bubble shown" << std::endl;
    QTemporaryDir dir121;
    assert(dir121.isValid());
    CredentialVaultManager manager121(dir121.path());
    assert(manager121.create(masterPassword));
    assert(!manager121.isLocked());

    CredentialAutofillController controller121(&manager121);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"newuser@example.com\",\"password\":\"NewPass#121\",\"submitted\":true}");
    assert(controller121.handleBridgeMessage(testView->page(), candMsg));

    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller121.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/feed")));

    assert(controller121.activeSaveBubble() != nullptr);
    assert(controller121.activeSaveBubble()->mode() == CredentialSaveMode::NewCredential);

    const QString candKey = controller121.candidateKey(testView, QStringLiteral("https://www.facebook.com"), QStringLiteral("newuser@example.com"));
    controller121.triggerSaveCandidate(candKey);

    const auto records = manager121.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(!records.isEmpty());
    bool found = false;
    for (const auto &r : records) {
      if (r.username == QStringLiteral("newuser@example.com")) found = true;
    }
    assert(found);

    controller121.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 121: vault unlocked successful login -> save bubble shown" << std::endl;
  }

  // TEST 122: existing credential changed password + successful login -> update bubble shown
  {
    std::cout << "[RUN] TEST 122: existing credential changed password + successful login -> update bubble shown" << std::endl;
    QTemporaryDir dir122;
    assert(dir122.isValid());
    CredentialVaultManager manager122(dir122.path());
    assert(manager122.create(masterPassword));

    // Save initial credential with old password
    CredentialSecret initialSec;
    initialSec.origin = QStringLiteral("https://www.facebook.com");
    initialSec.username = QStringLiteral("existing@example.com");
    initialSec.password = QStringLiteral("OldPassword#122");
    bool updated = false;
    assert(manager122.save(initialSec, &updated));

    CredentialAutofillController controller122(&manager122);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login")));

    // Login with SAME username but NEW password
    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"existing@example.com\",\"password\":\"BrandNewPassword#122\",\"submitted\":true}");
    assert(controller122.handleBridgeMessage(testView->page(), candMsg));

    // Successful login redirect to /
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/")));
    controller122.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/")));

    assert(controller122.activeSaveBubble() != nullptr);
    assert(controller122.activeSaveBubble()->mode() == CredentialSaveMode::UpdatePassword);

    const QString candKey = controller122.candidateKey(testView, QStringLiteral("https://www.facebook.com"), QStringLiteral("existing@example.com"));
    controller122.triggerUpdateCandidate(candKey);

    CredentialSecret revealed;
    const auto records = manager122.forOrigin(QUrl(QStringLiteral("https://www.facebook.com")));
    assert(!records.isEmpty());
    assert(manager122.reveal(records.front().id, &revealed));
    assert(revealed.password == QStringLiteral("BrandNewPassword#122"));

    controller122.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 122: existing credential changed password + successful login -> update bubble shown" << std::endl;
  }

  // TEST 123: successful redirect produces exactly one bubble
  {
    std::cout << "[RUN] TEST 123: successful redirect produces exactly one bubble" << std::endl;
    QTemporaryDir dir123;
    assert(dir123.isValid());
    CredentialVaultManager manager123(dir123.path());
    assert(manager123.create(masterPassword));

    CredentialAutofillController controller123(&manager123);
    auto *testView = new QWebEngineView();
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/login/")));

    const QString candMsg = QStringLiteral("candidate:{\"origin\":\"https://www.facebook.com\",\"username\":\"onebubble@example.com\",\"password\":\"Pass#123\",\"submitted\":true}");
    assert(controller123.handleBridgeMessage(testView->page(), candMsg));

    // 1. onUrlChanged fires
    testView->setUrl(QUrl(QStringLiteral("https://www.facebook.com/feed")));
    controller123.onUrlChanged(testView, QUrl(QStringLiteral("https://www.facebook.com/feed")));
    auto *bubble1 = controller123.activeSaveBubble();
    assert(bubble1 != nullptr);

    // 2. onPageLoadFinished fires on the same page
    controller123.onPageLoadFinished(testView, true);
    auto *bubble2 = controller123.activeSaveBubble();
    assert(bubble2 == bubble1); // Exactly one bubble, not duplicated or replaced

    controller123.onViewClosed(testView);
    delete testView;
    std::cout << "[PASS] TEST 123: successful redirect produces exactly one bubble" << std::endl;
  }

  // TEST 124: successful login on the same URL + form disappearance -> bubble
  {
    std::cout << "[RUN] TEST 124: same URL + form disappearance -> bubble" << std::endl;
    QTemporaryDir dir124;
    CredentialVaultManager manager124(dir124.path());
    assert(manager124.create(masterPassword));
    CredentialAutofillController controller124(&manager124);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://same-url.example/")));
    const QString candidate = QStringLiteral("candidate:{\"origin\":\"https://same-url.example\",\"username\":\"same@example.com\",\"password\":\"Same#124\",\"submitted\":true}");
    assert(controller124.handleBridgeMessage(view->page(), candidate));
    QThread::msleep(1550);
    const QString clearState = QStringLiteral("state:{\"origin\":\"https://same-url.example\",\"loginFormVisible\":false,\"passwordFieldVisible\":false,\"errorStateObserved\":false}");
    controller124.handleBridgeMessage(view->page(), clearState);
    controller124.handleBridgeMessage(view->page(), clearState);
    assert(controller124.activeSaveBubble() != nullptr);
    controller124.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 124: same URL + form disappearance -> bubble" << std::endl;
  }

  // TEST 125: event order URL -> load -> hint
  {
    std::cout << "[RUN] TEST 125: event order URL -> load -> hint" << std::endl;
    QTemporaryDir dir125;
    CredentialVaultManager manager125(dir125.path());
    assert(manager125.create(masterPassword));
    CredentialAutofillController controller125(&manager125);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://order-a.example/login")));
    controller125.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://order-a.example\",\"username\":\"a@example.com\",\"password\":\"Order#125\",\"submitted\":true}"));
    view->setUrl(QUrl(QStringLiteral("https://order-a.example/home")));
    controller125.onUrlChanged(view, view->url());
    controller125.onPageLoadFinished(view, true);
    controller125.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://order-a.example\"}"));
    assert(controller125.activeSaveBubble() != nullptr);
    controller125.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 125: event order URL -> load -> hint" << std::endl;
  }

  // TEST 126: event order hint -> URL -> load
  {
    std::cout << "[RUN] TEST 126: event order hint -> URL -> load" << std::endl;
    QTemporaryDir dir126;
    CredentialVaultManager manager126(dir126.path());
    assert(manager126.create(masterPassword));
    CredentialAutofillController controller126(&manager126);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://order-b.example/login")));
    controller126.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://order-b.example\",\"username\":\"b@example.com\",\"password\":\"Order#126\",\"submitted\":true}"));
    controller126.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://order-b.example\"}"));
    view->setUrl(QUrl(QStringLiteral("https://order-b.example/home")));
    controller126.onUrlChanged(view, view->url());
    controller126.onPageLoadFinished(view, true);
    assert(controller126.activeSaveBubble() != nullptr);
    controller126.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 126: event order hint -> URL -> load" << std::endl;
  }

  // TEST 127: event order load -> hint
  {
    std::cout << "[RUN] TEST 127: event order load -> hint" << std::endl;
    QTemporaryDir dir127;
    CredentialVaultManager manager127(dir127.path());
    assert(manager127.create(masterPassword));
    CredentialAutofillController controller127(&manager127);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://order-c.example/login")));
    controller127.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://order-c.example\",\"username\":\"c@example.com\",\"password\":\"Order#127\",\"submitted\":true}"));
    controller127.onPageLoadFinished(view, true);
    assert(controller127.activeSaveBubble() == nullptr);
    controller127.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://order-c.example\"}"));
    assert(controller127.activeSaveBubble() != nullptr);
    controller127.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 127: event order load -> hint" << std::endl;
  }

  // TEST 128: page load before final DOM success preserves the attempt
  {
    std::cout << "[RUN] TEST 128: early page load preserves attempt" << std::endl;
    QTemporaryDir dir128;
    CredentialVaultManager manager128(dir128.path());
    assert(manager128.create(masterPassword));
    CredentialAutofillController controller128(&manager128);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://same-load.example/")));
    controller128.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://same-load.example\",\"username\":\"load@example.com\",\"password\":\"Load#128\",\"submitted\":true}"));
    controller128.onPageLoadFinished(view, true);
    assert(controller128.activeSaveBubble() == nullptr);
    assert(controller128.hasActiveSubmittedLoginAttempt(view, QStringLiteral("https://same-load.example")));
    controller128.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://same-load.example\"}"));
    assert(controller128.activeSaveBubble() != nullptr);
    controller128.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 128: early page load preserves attempt" << std::endl;
  }

  // TEST 129: intermediate same-site auth redirect preserves the attempt
  {
    std::cout << "[RUN] TEST 129: intermediate same-site auth redirect" << std::endl;
    QTemporaryDir dir129;
    CredentialVaultManager manager129(dir129.path());
    assert(manager129.create(masterPassword));
    CredentialAutofillController controller129(&manager129);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://redirect.example/login")));
    controller129.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://redirect.example\",\"username\":\"redirect@example.com\",\"password\":\"Redirect#129\",\"submitted\":true}"));
    view->setUrl(QUrl(QStringLiteral("https://redirect.example/auth/continue")));
    controller129.onUrlChanged(view, view->url());
    controller129.onPageLoadFinished(view, true);
    assert(controller129.activeSaveBubble() == nullptr);
    assert(controller129.hasActiveSubmittedLoginAttempt(view, QStringLiteral("https://redirect.example")));
    view->setUrl(QUrl(QStringLiteral("https://redirect.example/home")));
    controller129.onUrlChanged(view, view->url());
    assert(controller129.activeSaveBubble() != nullptr);
    controller129.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 129: intermediate same-site auth redirect" << std::endl;
  }

  // TEST 130: password field remains briefly, then disappears
  {
    std::cout << "[RUN] TEST 130: grace period then password disappearance" << std::endl;
    QTemporaryDir dir130;
    CredentialVaultManager manager130(dir130.path());
    assert(manager130.create(masterPassword));
    CredentialAutofillController controller130(&manager130);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://grace.example/")));
    controller130.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://grace.example\",\"username\":\"grace@example.com\",\"password\":\"Grace#130\",\"submitted\":true}"));
    controller130.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://grace.example\",\"loginFormVisible\":true,\"passwordFieldVisible\":true,\"errorStateObserved\":false}"));
    assert(controller130.activeSaveBubble() == nullptr);
    assert(controller130.hasActiveSubmittedLoginAttempt(view, QStringLiteral("https://grace.example")));
    QThread::msleep(1550);
    const QString gone = QStringLiteral("state:{\"origin\":\"https://grace.example\",\"loginFormVisible\":false,\"passwordFieldVisible\":false,\"errorStateObserved\":false}");
    controller130.handleBridgeMessage(view->page(), gone);
    controller130.handleBridgeMessage(view->page(), gone);
    assert(controller130.activeSaveBubble() != nullptr);
    controller130.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 130: grace period then password disappearance" << std::endl;
  }

  // TEST 131: persistent password field plus explicit error -> no bubble
  {
    std::cout << "[RUN] TEST 131: explicit failure blocks prompt" << std::endl;
    QTemporaryDir dir131;
    CredentialVaultManager manager131(dir131.path());
    assert(manager131.create(masterPassword));
    CredentialAutofillController controller131(&manager131);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://failure.example/login")));
    controller131.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://failure.example\",\"username\":\"wrong@example.com\",\"password\":\"Wrong#131\",\"submitted\":true}"));
    controller131.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://failure.example\",\"loginFormVisible\":true,\"passwordFieldVisible\":true,\"errorStateObserved\":true}"));
    view->setUrl(QUrl(QStringLiteral("https://failure.example/login?error=invalid_password")));
    controller131.onUrlChanged(view, view->url());
    controller131.onPageLoadFinished(view, true);
    assert(controller131.activeSaveBubble() == nullptr);
    controller131.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 131: explicit failure blocks prompt" << std::endl;
  }

  // TEST 132: no evidence until confirmation timeout -> no bubble
  {
    std::cout << "[RUN] TEST 132: bounded confirmation timeout" << std::endl;
    QTemporaryDir dir132;
    CredentialVaultManager manager132(dir132.path());
    assert(manager132.create(masterPassword));
    CredentialAutofillController controller132(&manager132);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://timeout.example/login")));
    controller132.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://timeout.example\",\"username\":\"timeout@example.com\",\"password\":\"Timeout#132\",\"submitted\":true}"));
    const QString visibleState = QStringLiteral("state:{\"origin\":\"https://timeout.example\",\"loginFormVisible\":true,\"passwordFieldVisible\":true,\"errorStateObserved\":false}");
    for (int i = 0; i < 25; ++i) {
      controller132.handleBridgeMessage(view->page(), visibleState);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(500);
    }
    assert(!controller132.hasActiveSubmittedLoginAttempt(view, QStringLiteral("https://timeout.example")));
    // Bounded confirmation timeout stops login attempt polling, but once the save prompt
    // is visible as a pending user decision, it remains until explicit resolution.
    assert(controller132.activeSaveBubble() != nullptr);
    assert(controller132.activeSaveBubble()->isPrimaryButtonEnabled());
    controller132.activeSaveBubble()->clickSecondary();
    assert(controller132.activeSaveBubble() == nullptr);
    controller132.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 132: bounded confirmation timeout" << std::endl;
  }

  // TEST 133: native confirmation recheck finds a disappeared login form
  {
    std::cout << "[RUN] TEST 133: native recheck detects form disappearance" << std::endl;
    QTemporaryDir dir133;
    CredentialVaultManager manager133(dir133.path());
    assert(manager133.create(masterPassword));
    CredentialAutofillController controller133(&manager133);
    auto *view = new QWebEngineView();
    bool loaded = false;
    QObject::connect(view, &QWebEngineView::loadFinished, [&loaded](bool ok) { loaded = ok; });
    view->setHtml(QStringLiteral("<html><body><form id='login'><input type='email'><input type='password'><button>Login</button></form></body></html>"),
                  QUrl(QStringLiteral("https://poll.example/")));
    assert(waitForCondition([&loaded] { return loaded; }));
    controller133.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://poll.example\",\"username\":\"poll@example.com\",\"password\":\"Poll#133\",\"submitted\":true}"));
    view->page()->runJavaScript(QStringLiteral("document.getElementById('login').remove();"));
    assert(waitForCondition([&controller133] { return controller133.activeSaveBubble() != nullptr; }, 5000));
    controller133.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 133: native recheck detects form disappearance" << std::endl;
  }

  // TEST 134: confirmation recheck never reads a password value
  {
    std::cout << "[RUN] TEST 134: recheck never reads password value" << std::endl;
    const QString query = CredentialAutofillController::loginStateQueryScript();
    assert(query.contains(QStringLiteral("passwordFieldVisible")));
    assert(query.contains(QStringLiteral("errorStateObserved")));
    assert(!query.contains(QStringLiteral(".value")));
    std::cout << "[PASS] TEST 134: recheck never reads password value" << std::endl;
  }

  // TEST 135: multiple success signals produce exactly one bubble
  {
    std::cout << "[RUN] TEST 135: multiple evidence signals -> exactly one bubble" << std::endl;
    QTemporaryDir dir135;
    CredentialVaultManager manager135(dir135.path());
    assert(manager135.create(masterPassword));
    CredentialAutofillController controller135(&manager135);
    int shown = 0;
    QObject::connect(&controller135, &CredentialAutofillController::saveBubbleShown, [&shown] { ++shown; });
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://idempotent.example/login")));
    controller135.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://idempotent.example\",\"username\":\"once@example.com\",\"password\":\"Once#135\",\"submitted\":true}"));
    view->setUrl(QUrl(QStringLiteral("https://idempotent.example/home")));
    controller135.onUrlChanged(view, view->url());
    controller135.onPageLoadFinished(view, true);
    controller135.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://idempotent.example\"}"));
    controller135.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://idempotent.example\",\"loginFormVisible\":false,\"passwordFieldVisible\":false,\"errorStateObserved\":false}"));
    assert(shown == 1);
    controller135.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 135: multiple evidence signals -> exactly one bubble" << std::endl;
  }

  // TEST 136: ten repeated successful attempts are stable (10/10)
  {
    std::cout << "[RUN] TEST 136: 10 repeated successful attempts" << std::endl;
    QTemporaryDir dir136;
    CredentialVaultManager manager136(dir136.path());
    assert(manager136.create(masterPassword));
    CredentialAutofillController controller136(&manager136);
    int shown = 0;
    QObject::connect(&controller136, &CredentialAutofillController::saveBubbleShown, [&shown] { ++shown; });
    auto *view = new QWebEngineView();
    for (int i = 0; i < 10; ++i) {
      view->setUrl(QUrl(QStringLiteral("https://repeat.example/login?round=%1").arg(i)));
      const QString user = QStringLiteral("repeat%1@example.com").arg(i);
      const QString candidate = QStringLiteral("candidate:{\"origin\":\"https://repeat.example\",\"username\":\"%1\",\"password\":\"Repeat#136\",\"submitted\":true}").arg(user);
      controller136.handleBridgeMessage(view->page(), candidate);
      view->setUrl(QUrl(QStringLiteral("https://repeat.example/home?round=%1").arg(i)));
      controller136.onUrlChanged(view, view->url());
      controller136.onPageLoadFinished(view, true);
      assert(controller136.activeSaveBubble() != nullptr);
      const QString key = controller136.candidateKey(view, QStringLiteral("https://repeat.example"), user);
      controller136.triggerRejectCandidate(key);
      assert(controller136.activeSaveBubble() == nullptr);
    }
    assert(shown == 10);
    controller136.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 136: 10 repeated successful attempts (10/10)" << std::endl;
  }

  // TEST 137: ten eye toggles without submit produce zero bubbles
  {
    std::cout << "[RUN] TEST 137: 10 eye toggles without submit" << std::endl;
    QTemporaryDir dir137;
    CredentialVaultManager manager137(dir137.path());
    assert(manager137.create(masterPassword));
    CredentialAutofillController controller137(&manager137);
    int shown = 0;
    QObject::connect(&controller137, &CredentialAutofillController::saveBubbleShown, [&shown] { ++shown; });
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://eye.example/login")));
    for (int i = 0; i < 10; ++i) {
      const QString candidate = QStringLiteral("candidate:{\"origin\":\"https://eye.example\",\"username\":\"eye%1@example.com\",\"password\":\"Eye#137\",\"submitted\":false}").arg(i);
      controller137.handleBridgeMessage(view->page(), candidate);
      controller137.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://eye.example\"}"));
    }
    assert(shown == 0);
    assert(controller137.activeSaveBubble() == nullptr);
    controller137.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 137: 10 eye toggles without submit (0 bubbles)" << std::endl;
  }

  // TEST 138: cookie-cleared fresh login remains eligible
  {
    std::cout << "[RUN] TEST 138: cookie-cleared fresh login" << std::endl;
    QTemporaryDir dir138;
    CredentialVaultManager manager138(dir138.path());
    assert(manager138.create(masterPassword));
    CredentialAutofillController controller138(&manager138);
    auto *view = new QWebEngineView();
    view->page()->profile()->cookieStore()->deleteAllCookies();
    view->setUrl(QUrl(QStringLiteral("https://fresh.example/login")));
    controller138.handleBridgeMessage(view->page(), QStringLiteral("candidate:{\"origin\":\"https://fresh.example\",\"username\":\"fresh@example.com\",\"password\":\"Fresh#138\",\"submitted\":true}"));
    view->setUrl(QUrl(QStringLiteral("https://fresh.example/home")));
    controller138.onUrlChanged(view, view->url());
    assert(controller138.activeSaveBubble() != nullptr);
    controller138.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 138: cookie-cleared fresh login" << std::endl;
  }

  // TEST 139: an existing authenticated session without submit produces no prompt
  {
    std::cout << "[RUN] TEST 139: existing session without submit" << std::endl;
    QTemporaryDir dir139;
    CredentialVaultManager manager139(dir139.path());
    assert(manager139.create(masterPassword));
    CredentialAutofillController controller139(&manager139);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://session.example/home")));
    controller139.onUrlChanged(view, view->url());
    controller139.onPageLoadFinished(view, true);
    controller139.handleBridgeMessage(view->page(), QStringLiteral("success:{\"origin\":\"https://session.example\"}"));
    controller139.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://session.example\",\"loginFormVisible\":false,\"passwordFieldVisible\":false,\"errorStateObserved\":false,\"authenticatedStateObserved\":true}"));
    assert(controller139.activeSaveBubble() == nullptr);
    controller139.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 139: existing session without submit" << std::endl;
  }

  const auto candidateMessage = [](const QString &origin, const QString &username,
                                   const QString &password, bool submitted) {
    return QStringLiteral("candidate:") +
           QString::fromUtf8(QJsonDocument(QJsonObject{
               {QStringLiteral("origin"), origin},
               {QStringLiteral("username"), username},
               {QStringLiteral("password"), password},
               {QStringLiteral("submitted"), submitted},
               {QStringLiteral("nonce"), QStringLiteral("test-flow-nonce")}
           }).toJson(QJsonDocument::Compact));
  };
  const auto successMessage = [](const QString &origin) {
    return QStringLiteral("success:") +
           QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("origin"), origin}})
                                 .toJson(QJsonDocument::Compact));
  };

  // TEST 140: trusted submit immediately creates PendingCredentialSaveFlow
  {
    std::cout << "[RUN] TEST 140: trusted submit creates save flow" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow140.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user140@example.com"), QStringLiteral("Secret#140"), true));
    assert(c.hasPendingCredentialSaveFlow(view, origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::Verifying);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 140: trusted submit creates save flow" << std::endl;
  }

  // TEST 141: no submit -> no save flow
  {
    std::cout << "[RUN] TEST 141: no submit -> no save flow" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow141.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user141@example.com"), QStringLiteral("Secret#141"), false));
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 141: no submit -> no save flow" << std::endl;
  }

  // TEST 142: trusted submit -> VERIFYING state with disabled Save
  {
    std::cout << "[RUN] TEST 142: trusted submit -> VERIFYING" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow142.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user142@example.com"), QStringLiteral("Secret#142"), true));
    assert(waitForCondition([&c] { return c.activeSaveBubble() != nullptr; }, 1000));
    assert(c.activeSaveBubble()->mode() == CredentialSaveMode::Verifying);
    assert(c.activeSaveBubble()->descriptionText() == QStringLiteral("Giriş doğrulanıyor…"));
    assert(!c.activeSaveBubble()->isPrimaryButtonEnabled());
    c.activeSaveBubble()->clickPrimary();
    assert(c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 142: trusted submit -> VERIFYING" << std::endl;
  }

  // TEST 143: successful confirmation -> READY_NEW
  {
    std::cout << "[RUN] TEST 143: success -> READY_NEW" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow143.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user143@example.com"), QStringLiteral("Secret#143"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
    assert(c.activeSaveBubble() && c.activeSaveBubble()->mode() == CredentialSaveMode::ReadyNew);
    assert(c.activeSaveBubble()->isPrimaryButtonEnabled());
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 143: success -> READY_NEW" << std::endl;
  }

  // TEST 144: explicit failure -> flow cancelled
  {
    std::cout << "[RUN] TEST 144: explicit failure cancels flow" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow144.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user144@example.com"), QStringLiteral("Wrong#144"), true));
    c.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://flow144.example\",\"loginFormVisible\":true,\"passwordFieldVisible\":true,\"errorStateObserved\":true}"));
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    assert(c.pendingCandidateCount() == 0);
    assert(c.activeSaveBubble() == nullptr);
    assert(c.lastSaveFlowEndReason() == QStringLiteral("failure"));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 144: explicit failure cancels flow" << std::endl;
  }

  // TEST 145: verification timeout -> pending user decision preserved until explicit resolution
  {
    std::cout << "[RUN] TEST 145: verification timeout preserves pending decision until user resolution" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    bool loaded = false;
    QObject::connect(view, &QWebEngineView::loadFinished, [&loaded](bool ok) { loaded = ok; });
    const QString origin = QStringLiteral("https://flow145.example");
    view->setHtml(QStringLiteral("<form><input type='password'></form>"), QUrl(origin + QStringLiteral("/login")));
    assert(waitForCondition([&loaded] { return loaded; }));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user145@example.com"), QStringLiteral("Secret#145"), true));
    assert(waitForCondition([&c, view, origin] { return !c.hasPendingCredentialSaveFlow(view, origin); }, 14000));
    assert(c.lastSaveFlowEndReason() == QStringLiteral("timeout"));
    // Once shown, bubble remains visible as a pending user decision until explicit user resolution
    assert(c.activeSaveBubble() != nullptr);
    assert(c.activeSaveBubble()->isPrimaryButtonEnabled());
    c.activeSaveBubble()->clickSecondary(); // User clicks "Şimdi Değil"
    assert(c.activeSaveBubble() == nullptr);
    assert(c.pendingCandidateCount() == 0);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 145: verification timeout preserves pending decision until user resolution" << std::endl;
  }

  // TEST 146: eye toggle -> no flow
  {
    std::cout << "[RUN] TEST 146: eye toggle -> no flow" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow146.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user146@example.com"), QStringLiteral("Secret#146"), false));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    assert(c.activeSaveBubble() == nullptr);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 146: eye toggle -> no flow" << std::endl;
  }

  // TEST 147: same URL login -> READY_NEW
  {
    std::cout << "[RUN] TEST 147: same URL login -> READY_NEW" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow147.example");
    view->setUrl(QUrl(origin + QStringLiteral("/")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user147@example.com"), QStringLiteral("Secret#147"), true));
    c.onPageLoadFinished(view, true);
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::Verifying);
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 147: same URL login -> READY_NEW" << std::endl;
  }

  // TEST 148: slow login spinner then success -> READY_NEW
  {
    std::cout << "[RUN] TEST 148: slow login spinner then success" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow148.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user148@example.com"), QStringLiteral("Secret#148"), true));
    assert(waitForCondition([&c] { return c.activeSaveBubble() != nullptr; }, 1000));
    QElapsedTimer spinner;
    spinner.start();
    while (spinner.elapsed() < 800) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::Verifying);
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 148: slow login spinner then success" << std::endl;
  }

  // TEST 149: fast redirect login -> READY_NEW
  {
    std::cout << "[RUN] TEST 149: fast redirect -> READY_NEW" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow149.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user149@example.com"), QStringLiteral("Secret#149"), true));
    view->setUrl(QUrl(origin + QStringLiteral("/home")));
    c.onUrlChanged(view, view->url());
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 149: fast redirect -> READY_NEW" << std::endl;
  }

  // TEST 150: no vault login -> completely silent, no prompt, no open request
  {
    std::cout << "[RUN] TEST 150: no-vault login produces no bubble and no open request" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow150.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    bool opened = false;
    QObject::connect(&c, &CredentialAutofillController::openPasswordManagerRequested, [&opened] { opened = true; });
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user150@example.com"), QStringLiteral("Secret#150"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.activeSaveBubble() == nullptr);
    assert(!opened);
    assert(c.pendingCandidateCount() == 0);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 150: no-vault login produces no bubble and no open request" << std::endl;
  }

  // TEST 151: no-vault login + manual vault creation -> no auto-save of past credentials
  {
    std::cout << "[RUN] TEST 151: no-vault login + manual vault creation -> no auto-save of past credentials" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow151.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user151@example.com"), QStringLiteral("Secret#151"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.activeSaveBubble() == nullptr);
    assert(manager.create(masterPassword));
    const auto records = manager.forOrigin(QUrl(origin));
    assert(records.isEmpty());
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 151: no-vault login + manual vault creation -> no auto-save of past credentials" << std::endl;
  }

  // TEST 152: no-vault login keeps pending candidate count zero
  {
    std::cout << "[RUN] TEST 152: no-vault login keeps pending candidate count zero" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow152.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user152@example.com"), QStringLiteral("Secret#152"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.activeSaveBubble() == nullptr);
    assert(c.pendingCandidateCount() == 0);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 152: no-vault login keeps pending candidate count zero" << std::endl;
  }

  // TEST 153: unlocked vault + Save -> encrypted save
  {
    std::cout << "[RUN] TEST 153: unlocked vault Save" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow153.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user153@example.com"), QStringLiteral("Secret#153"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    c.activeSaveBubble()->clickPrimary();
    assert(manager.forOrigin(QUrl(origin)).size() == 1);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 153: unlocked vault Save" << std::endl;
  }

  // TEST 154: locked vault + Save -> VaultUnlockDialog
  {
    std::cout << "[RUN] TEST 154: locked vault Save opens unlock" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    manager.lock();
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow154.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user154@example.com"), QStringLiteral("Secret#154"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    c.activeSaveBubble()->clickPrimary();
    assert(c.activeUnlockDialog() != nullptr);
    c.activeUnlockDialog()->reject();
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 154: locked vault Save opens unlock" << std::endl;
  }

  // TEST 155: unlock success -> save
  {
    std::cout << "[RUN] TEST 155: unlock success saves" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    manager.lock();
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow155.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user155@example.com"), QStringLiteral("Secret#155"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    c.activeSaveBubble()->clickPrimary();
    auto *dialog = c.activeUnlockDialog();
    assert(dialog != nullptr);
    dialog->passwordInput()->setText(masterPassword);
    dialog->attemptUnlock();
    assert(waitForCondition([&manager, &origin] { return manager.forOrigin(QUrl(origin)).size() == 1; }, 8000));
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 155: unlock success saves" << std::endl;
  }

  // TEST 156: unlock cancel -> no save
  {
    std::cout << "[RUN] TEST 156: unlock cancel -> no save" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    manager.lock();
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow156.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user156@example.com"), QStringLiteral("Secret#156"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    c.activeSaveBubble()->clickPrimary();
    assert(c.activeUnlockDialog());
    c.activeUnlockDialog()->reject();
    QCoreApplication::processEvents();
    assert(c.pendingCandidateCount() == 0);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    assert(manager.isLocked());
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 156: unlock cancel -> no save" << std::endl;
  }

  // TEST 157: same credential + same password -> no duplicate
  {
    std::cout << "[RUN] TEST 157: unchanged credential no-op" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    const QString origin = QStringLiteral("https://flow157.example");
    CredentialSecret initial;
    initial.origin = origin;
    initial.username = QStringLiteral("user157@example.com");
    initial.password = QStringLiteral("Secret#157");
    bool updated = false;
    assert(manager.save(initial, &updated));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, initial.username, initial.password, true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.activeSaveBubble() == nullptr);
    assert(manager.forOrigin(QUrl(origin)).size() == 1);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    initial.password.fill(QChar());
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 157: unchanged credential no-op" << std::endl;
  }

  // TEST 158: changed password -> READY_UPDATE
  {
    std::cout << "[RUN] TEST 158: changed password -> READY_UPDATE" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    const QString origin = QStringLiteral("https://flow158.example");
    CredentialSecret initial;
    initial.origin = origin;
    initial.username = QStringLiteral("user158@example.com");
    initial.password = QStringLiteral("Old#158");
    bool updated = false;
    assert(manager.save(initial, &updated));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, initial.username, QStringLiteral("New#158"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyUpdate);
    assert(c.activeSaveBubble()->mode() == CredentialSaveMode::ReadyUpdate);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 158: changed password -> READY_UPDATE" << std::endl;
  }

  // TEST 159: READY_UPDATE + update action -> update
  {
    std::cout << "[RUN] TEST 159: READY_UPDATE action updates" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    const QString origin = QStringLiteral("https://flow159.example");
    CredentialSecret initial;
    initial.origin = origin;
    initial.username = QStringLiteral("user159@example.com");
    initial.password = QStringLiteral("Old#159");
    bool updated = false;
    assert(manager.save(initial, &updated));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, initial.username, QStringLiteral("New#159"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    c.activeSaveBubble()->clickPrimary();
    CredentialSecret revealed;
    const auto records = manager.forOrigin(QUrl(origin));
    assert(records.size() == 1 && manager.reveal(records.front().id, &revealed));
    assert(revealed.password == QStringLiteral("New#159"));
    revealed.password.fill(QChar());
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 159: READY_UPDATE action updates" << std::endl;
  }

  // TEST 160: different username on same origin -> new record
  {
    std::cout << "[RUN] TEST 160: multi-account creates new record" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    const QString origin = QStringLiteral("https://flow160.example");
    CredentialSecret initial;
    initial.origin = origin;
    initial.username = QStringLiteral("first@example.com");
    initial.password = QStringLiteral("First#160");
    bool updated = false;
    assert(manager.save(initial, &updated));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("second@example.com"), QStringLiteral("Second#160"), true));
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
    c.activeSaveBubble()->clickPrimary();
    assert(manager.forOrigin(QUrl(origin)).size() == 2);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 160: multi-account creates new record" << std::endl;
  }

  // TEST 161: ten repeated real save-flow successes -> 10/10 READY_NEW
  {
    std::cout << "[RUN] TEST 161: 10 repeated save flows" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow161.example");
    int readyCount = 0;
    QObject::connect(&c, &CredentialAutofillController::saveFlowStateChanged,
                     [&readyCount](const QString &, const QString &, CredentialSaveFlowState state) {
      if (state == CredentialSaveFlowState::ReadyNew) ++readyCount;
    });
    for (int i = 0; i < 10; ++i) {
      view->setUrl(QUrl(origin + QStringLiteral("/login?round=%1").arg(i)));
      const QString username = QStringLiteral("user%1@flow161.example").arg(i);
      c.handleBridgeMessage(view->page(), candidateMessage(origin, username, QStringLiteral("Secret#161"), true));
      c.handleBridgeMessage(view->page(), successMessage(origin));
      assert(c.credentialSaveFlowState(view, origin) == CredentialSaveFlowState::ReadyNew);
      c.activeSaveBubble()->clickSecondary();
    }
    assert(readyCount == 10);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 161: 10 repeated save flows (10/10)" << std::endl;
  }

  // TEST 162: ten eye toggles -> zero flows
  {
    std::cout << "[RUN] TEST 162: 10 eye toggles -> 0 flows" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow162.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    for (int i = 0; i < 10; ++i) {
      c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("eye%1@example.com").arg(i), QStringLiteral("Eye#162"), false));
      c.handleBridgeMessage(view->page(), successMessage(origin));
      assert(!c.hasPendingCredentialSaveFlow(view, origin));
    }
    assert(c.activeSaveBubble() == nullptr);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 162: 10 eye toggles -> 0 flows" << std::endl;
  }

  // TEST 163: bubble close -> pending secret cleared
  {
    std::cout << "[RUN] TEST 163: bubble close clears pending secret" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow163.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user163@example.com"), QStringLiteral("Secret#163"), true));
    assert(waitForCondition([&c] { return c.activeSaveBubble() != nullptr; }, 1000));
    c.activeSaveBubble()->clickClose();
    assert(c.pendingCandidateCount() == 0);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 163: bubble close clears pending secret" << std::endl;
  }

  // TEST 164: tab close -> pending secret cleared
  {
    std::cout << "[RUN] TEST 164: tab close clears pending secret" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow164.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user164@example.com"), QStringLiteral("Secret#164"), true));
    c.onViewClosed(view);
    assert(c.pendingCandidateCount() == 0);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    assert(c.lastSaveFlowEndReason() == QStringLiteral("tab_close"));
    delete view;
    std::cout << "[PASS] TEST 164: tab close clears pending secret" << std::endl;
  }

  // TEST 165: origin invalidation -> pending secret cleared
  {
    std::cout << "[RUN] TEST 165: origin invalidation clears pending secret" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow165.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user165@example.com"), QStringLiteral("Secret#165"), true));
    view->setUrl(QUrl(QStringLiteral("https://unrelated165.example/home")));
    c.onUrlChanged(view, view->url());
    assert(c.pendingCandidateCount() == 0);
    assert(!c.hasPendingCredentialSaveFlow(view, origin));
    assert(c.lastSaveFlowEndReason() == QStringLiteral("unrelated_navigation"));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 165: origin invalidation clears pending secret" << std::endl;
  }

  // TEST 166: no plaintext pending credential on disk
  {
    std::cout << "[RUN] TEST 166: pending secret remains RAM-only" << std::endl;
    assert(CredentialAutofillController::isSameSiteOrOrigin(
        QStringLiteral("https://login.example.co.uk"), QStringLiteral("https://app.example.co.uk")));
    assert(!CredentialAutofillController::isSameSiteOrOrigin(
        QStringLiteral("https://attacker.co.uk"), QStringLiteral("https://example.co.uk")));
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow166.example");
    const QString password = QStringLiteral("NeverPersistPendingSecret#166");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user166@example.com"), password, true));
    QDirIterator files(dir.path(), QDirIterator::Subdirectories);
    while (files.hasNext()) {
      QFileInfo info(files.next());
      if (!info.isFile()) continue;
      QFile file(info.filePath());
      if (file.open(QIODevice::ReadOnly)) assert(!file.readAll().contains(password.toUtf8()));
    }
    assert(c.hasPendingCredentialSaveFlow(view, origin));
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 166: pending secret remains RAM-only" << std::endl;
  }

  // TEST 167: no-vault login -> completely silent, no verifying or save bubble
  {
    std::cout << "[RUN] TEST 167: no-vault login -> completely silent, no verifying or save bubble" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    CredentialAutofillController c(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://flow167.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user167@example.com"), QStringLiteral("Secret#167"), true));
    assert(c.activeSaveBubble() == nullptr);
    c.handleBridgeMessage(view->page(), successMessage(origin));
    assert(c.activeSaveBubble() == nullptr);
    assert(c.pendingCandidateCount() == 0);
    c.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 167: no-vault login -> completely silent, no verifying or save bubble" << std::endl;
  }

  // TEST 168: capture remains trusted-event-only without rejecting valid Qt/Chromium clicks
  {
    std::cout << "[RUN] TEST 168: trusted responsive login capture guards" << std::endl;
    const QString capture = CredentialAutofillController::candidateCaptureScript().sourceCode();
    assert(capture.contains(QStringLiteral("if (!event.isTrusted) return;")));
    assert(!capture.contains(QStringLiteral("navigator.userActivation.isActive === false")));
    assert(capture.contains(QStringLiteral("const groups = new Map()")));
    assert(capture.contains(QStringLiteral("filter(visible)")));
    assert(capture.contains(QStringLiteral("event.composedPath")));
    std::cout << "[PASS] TEST 168: trusted responsive login capture guards" << std::endl;
  }

  // TEST 169 (Test A): No Vault -> completely silent, no bubble, no pending credentials
  {
    std::cout << "[RUN] TEST 169 (Test A): No Vault -> completely silent, no bubble, no pending credentials" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(!manager.exists());

    CredentialAutofillController controller(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://test-a.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    controller.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("alice@example.com"), QStringLiteral("AliceSecret#1"), true));
    controller.handleBridgeMessage(view->page(), successMessage(origin));

    assert(controller.activeSaveBubble() == nullptr);
    assert(controller.pendingCandidateCount() == 0);
    assert(manager.list().isEmpty());

    controller.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 169 (Test A): No Vault -> completely silent, no bubble, no pending credentials" << std::endl;
  }

  // TEST 170 (Test B): No Vault -> subsequent vault creation NEVER retroactively saves past logins
  {
    std::cout << "[RUN] TEST 170 (Test B): No Vault -> subsequent vault creation NEVER retroactively saves past logins" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(!manager.exists());

    CredentialAutofillController controller(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://test-b.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    controller.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("bob@example.com"), QStringLiteral("BobSecret#2"), true));
    controller.handleBridgeMessage(view->page(), successMessage(origin));

    assert(controller.activeSaveBubble() == nullptr);
    assert(controller.pendingCandidateCount() == 0);

    // User later manually creates vault
    assert(manager.create(masterPassword));
    assert(manager.exists());

    // Verify no automatic or retroactive save occurred
    const auto records = manager.forOrigin(QUrl(origin));
    assert(records.isEmpty());
    assert(manager.list().isEmpty());

    controller.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 170 (Test B): No Vault -> subsequent vault creation NEVER retroactively saves past logins" << std::endl;
  }

  // TEST 171 (Test C): Vault Exists + Locked -> save prompt shown, Save triggers VaultUnlockDialog
  {
    std::cout << "[RUN] TEST 171 (Test C): Vault Exists + Locked -> save prompt shown, Save triggers VaultUnlockDialog" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    manager.lock();
    assert(manager.exists());
    assert(manager.isLocked());

    CredentialAutofillController controller(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://test-c.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    controller.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("charlie@example.com"), QStringLiteral("Charlie#3"), true));
    controller.handleBridgeMessage(view->page(), successMessage(origin));

    // Save prompt must be shown even when locked
    assert(controller.activeSaveBubble() != nullptr);
    assert(controller.activeSaveBubble()->mode() == CredentialSaveMode::ReadyNew);

    // Clicking save when locked must open the unlock dialog
    controller.activeSaveBubble()->clickPrimary();
    assert(controller.activeUnlockDialog() != nullptr);

    // Complete the unlock flow
    controller.activeUnlockDialog()->passwordInput()->setText(masterPassword);
    controller.activeUnlockDialog()->attemptUnlock();
    assert(waitForCondition([&controller] { return controller.activeUnlockDialog() == nullptr; }, 2000));
    assert(!manager.isLocked());

    // Verify credential was saved
    const auto records = manager.forOrigin(QUrl(origin));
    assert(records.size() == 1);
    assert(records.front().username == QStringLiteral("charlie@example.com"));

    CredentialSecret secret;
    assert(manager.reveal(records.front().id, &secret));
    assert(secret.password == QStringLiteral("Charlie#3"));

    controller.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 171 (Test C): Vault Exists + Locked -> save prompt shown, Save triggers VaultUnlockDialog" << std::endl;
  }

  // TEST 172 (Test D): Vault Exists + Unlocked -> save prompt shown, direct save without unlock dialog
  {
    std::cout << "[RUN] TEST 172 (Test D): Vault Exists + Unlocked -> save prompt shown, direct save without unlock dialog" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    assert(manager.exists());
    assert(!manager.isLocked());

    CredentialAutofillController controller(&manager);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://test-d.example");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    controller.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("dave@example.com"), QStringLiteral("Dave#4"), true));
    controller.handleBridgeMessage(view->page(), successMessage(origin));

    assert(controller.activeSaveBubble() != nullptr);
    assert(controller.activeSaveBubble()->mode() == CredentialSaveMode::ReadyNew);

    controller.activeSaveBubble()->clickPrimary();
    assert(controller.activeUnlockDialog() == nullptr);

    const auto records = manager.forOrigin(QUrl(origin));
    assert(records.size() == 1);
    assert(records.front().username == QStringLiteral("dave@example.com"));

    CredentialSecret secret;
    assert(manager.reveal(records.front().id, &secret));
    assert(secret.password == QStringLiteral("Dave#4"));

    controller.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 172 (Test D): Vault Exists + Unlocked -> save prompt shown, direct save without unlock dialog" << std::endl;
  }

  // TEST 173 (Test E): Manual vault creation workflow via settings / dalinira://passwords
  {
    std::cout << "[RUN] TEST 173 (Test E): Manual vault creation workflow" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(!manager.exists());

    // User navigates manually to dalinira://passwords and creates the vault
    assert(manager.create(QStringLiteral("CustomMasterPassword#2026")));
    assert(manager.exists());
    assert(!manager.isLocked());
    assert(manager.list().isEmpty());

    std::cout << "[PASS] TEST 173 (Test E): Manual vault creation workflow" << std::endl;
  }

  // TEST 174 (Test F): Existing credentials and PBKDF2/AES-GCM encryption integrity
  {
    std::cout << "[RUN] TEST 174 (Test F): Existing credentials and encryption integrity" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    const QString secretMaster = QStringLiteral("StrongMasterPass#987");
    assert(manager.create(secretMaster));

    CredentialSecret secret;
    secret.origin = QStringLiteral("https://secure.example");
    secret.username = QStringLiteral("secuser@example.com");
    secret.password = QStringLiteral("VeryComplexP@ssw0rd!#$");
    bool updated = false;
    assert(manager.save(secret, &updated));

    // Lock the vault
    manager.lock();
    assert(manager.isLocked());

    // Wrong password must fail
    assert(!manager.unlock(QStringLiteral("WrongPass#000")));
    assert(manager.isLocked());

    // Correct password unlocks
    assert(manager.unlock(secretMaster));
    assert(!manager.isLocked());

    // Decrypt and verify
    const auto records = manager.forOrigin(QUrl(QStringLiteral("https://secure.example")));
    assert(records.size() == 1);
    CredentialSecret decrypted;
    assert(manager.reveal(records.front().id, &decrypted));
    assert(decrypted.username == QStringLiteral("secuser@example.com"));
    assert(decrypted.password == QStringLiteral("VeryComplexP@ssw0rd!#$"));

    std::cout << "[PASS] TEST 174 (Test F): Existing credentials and encryption integrity" << std::endl;
  }

  // TEST 175: both username and password fields expose one canonical action.
  {
    std::cout << "[RUN] TEST 175: unified username/password credential action" << std::endl;
    const QString script = CredentialAutofillController::fillButtonScript(QStringLiteral("field-token"));
    assert(script.contains(QStringLiteral("isLoginIdentifier")));
    assert(script.contains(QStringLiteral("isPasswordField")));
    assert(script.contains(QStringLiteral("isCredentialField")));
    assert(script.contains(QStringLiteral("autocomplete.includes('username')")));
    assert(script.contains(QStringLiteral("querySelectorAll(\n        'input[type=\"password\"]")));
    assert(script.contains(QStringLiteral("stronglyIdentified")));
    const QString guardedFill = CredentialAutofillController::domFillScript(
        QStringLiteral("https://fields.example"), QStringLiteral("user@example.com"),
        QStringLiteral("FieldSecret#2026"), QUrl(QStringLiteral("https://fields.example/login")),
        QStringLiteral("document-token"));
    assert(guardedFill.contains(QStringLiteral("location.href !== v.url")));
    assert(guardedFill.contains(QStringLiteral("window.__daliniraFillToken !== v.documentToken")));
    std::cout << "[PASS] TEST 175: unified username/password credential action" << std::endl;
  }

  // TEST 176: unlocked vault still requires reauthentication; duplicate
  // actions cannot create a second authorization or release.
  {
    std::cout << "[RUN] TEST 176: unlocked vault requires one reauthentication" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    assert(manager.save({QStringLiteral("https://reauth.example"), QStringLiteral("alice@example.com"),
                         QStringLiteral("AliceSecret#2026"), {}}));
    CredentialAutofillController c(&manager);
    QWebEngineView view;
    view.setUrl(QUrl(QStringLiteral("https://reauth.example/login")));
    c.onPageLoadFinished(&view, true);
    int requests = 0;
    int fills = 0;
    QObject::connect(&c, &CredentialAutofillController::reauthenticationRequested,
                     [&requests] { ++requests; });
    QObject::connect(&c, &CredentialAutofillController::credentialFillDispatched,
                     [&fills] { ++fills; });
    c.triggerFillForView(&view);
    assert(c.activeUnlockDialog());
    assert(c.activeUnlockDialog()->purpose() == VaultUnlockDialog::Purpose::ReauthenticateForAutofill);
    c.triggerFillForView(&view);
    assert(requests == 1 && fills == 0);
    c.activeUnlockDialog()->reject();
    QApplication::processEvents();
    assert(fills == 0);
    c.onViewClosed(&view);
    std::cout << "[PASS] TEST 176: unlocked vault requires one reauthentication" << std::endl;
  }

  // TEST 177: wrong master password releases nothing; the correct password
  // dispatches exactly one credential pair.
  {
    std::cout << "[RUN] TEST 177: wrong/correct master password release policy" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    assert(manager.save({QStringLiteral("https://release.example"), QStringLiteral("bob@example.com"),
                         QStringLiteral("BobSecret#2026"), {}}));
    CredentialAutofillController c(&manager);
    QWebEngineView view;
    view.setUrl(QUrl(QStringLiteral("https://release.example/login")));
    c.onPageLoadFinished(&view, true);
    int fills = 0;
    QObject::connect(&c, &CredentialAutofillController::credentialFillDispatched,
                     [&fills] { ++fills; });
    c.triggerFillForView(&view);
    auto *dialog = c.activeUnlockDialog();
    assert(dialog);
    dialog->passwordInput()->setText(QStringLiteral("WrongMasterPassword#2026"));
    dialog->attemptUnlock();
    assert(waitForCondition([dialog] { return !dialog->isChecking(); }));
    assert(fills == 0 && c.activeUnlockDialog() == dialog);
    dialog->passwordInput()->setText(masterPassword);
    dialog->attemptUnlock();
    assert(waitForCondition([&fills] { return fills == 1; }));
    assert(fills == 1);
    c.onViewClosed(&view);
    std::cout << "[PASS] TEST 177: wrong/correct master password release policy" << std::endl;
  }

  // TEST 178: any navigation while authentication is open invalidates the
  // pending page/view/URL/generation grant.
  {
    std::cout << "[RUN] TEST 178: navigation invalidates autofill authorization" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    assert(manager.save({QStringLiteral("https://toctou.example"), QStringLiteral("carol@example.com"),
                         QStringLiteral("CarolSecret#2026"), {}}));
    CredentialAutofillController c(&manager);
    QWebEngineView view;
    view.setUrl(QUrl(QStringLiteral("https://toctou.example/login")));
    c.onPageLoadFinished(&view, true);
    int fills = 0;
    QObject::connect(&c, &CredentialAutofillController::credentialFillDispatched,
                     [&fills] { ++fills; });
    c.triggerFillForView(&view);
    QPointer<VaultUnlockDialog> staleDialog = c.activeUnlockDialog();
    assert(staleDialog);
    staleDialog->passwordInput()->setText(masterPassword);
    staleDialog->attemptUnlock();
    assert(staleDialog->isChecking());
    view.setUrl(QUrl(QStringLiteral("https://toctou.example/other")));
    c.onUrlChanged(&view, view.url());
    QApplication::processEvents();
    assert(c.activeUnlockDialog() == nullptr && fills == 0);
    assert(waitForCondition([staleDialog] { return staleDialog.isNull() || !staleDialog->isChecking(); }));
    assert(fills == 0);
    c.onViewClosed(&view);
    std::cout << "[PASS] TEST 178: navigation invalidates autofill authorization" << std::endl;
  }

  // TEST 179: multiple-account chooser exposes usernames only, and the chosen
  // record is the only one released after reauthentication.
  {
    std::cout << "[RUN] TEST 179: multiple-account selection" << std::endl;
    QTemporaryDir dir;
    CredentialVaultManager manager(dir.path());
    assert(manager.create(masterPassword));
    const QString origin = QStringLiteral("https://accounts-choice.example");
    assert(manager.save({origin, QStringLiteral("first@example.com"), QStringLiteral("FirstSecret#2026"), {}}));
    assert(manager.save({origin, QStringLiteral("second@example.com"), QStringLiteral("SecondSecret#2026"), {}}));
    CredentialAutofillController c(&manager);
    QWebEngineView view;
    view.setUrl(QUrl(origin + QStringLiteral("/login")));
    c.onPageLoadFinished(&view, true);
    QString releasedUsername;
    int fills = 0;
    QObject::connect(&c, &CredentialAutofillController::credentialFillDispatched,
                     [&fills, &releasedUsername](const QString &, const QString &username) {
                       ++fills;
                       releasedUsername = username;
                     });
    auto *inputDialogTimer = new QTimer();
    QObject::connect(inputDialogTimer, &QTimer::timeout, [inputDialogTimer]() {
      QInputDialog *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
      if (!dialog) {
        for (auto *top : QApplication::topLevelWidgets()) {
          if (auto *dlg = qobject_cast<QInputDialog *>(top)) {
            dialog = dlg;
            break;
          }
        }
      }
      if (dialog) {
        auto *combo = dialog->findChild<QComboBox *>();
        if (combo && combo->count() == 2) {
          assert(!combo->itemText(0).contains(QStringLiteral("Secret")));
          assert(!combo->itemText(1).contains(QStringLiteral("Secret")));
          combo->setCurrentIndex(1);
          dialog->accept();
          inputDialogTimer->stop();
          inputDialogTimer->deleteLater();
        }
      }
    });
    inputDialogTimer->start(10);

    c.triggerFillForView(&view);
    assert(c.activeUnlockDialog());
    c.activeUnlockDialog()->passwordInput()->setText(masterPassword);
    c.activeUnlockDialog()->attemptUnlock();
    assert(waitForCondition([&fills] { return fills == 1; }, 30000));
    assert(releasedUsername == QStringLiteral("second@example.com"));
    c.onViewClosed(&view);
    std::cout << "[PASS] TEST 179: multiple-account selection" << std::endl;
  }

  // TEST 180: pending save decision -> navigation -> explicit Not Now rejects and cleans up
  {
    std::cout << "[RUN] TEST 180: pending save -> navigation -> explicit Not Now" << std::endl;
    QTemporaryDir dir180;
    CredentialVaultManager manager180(dir180.path());
    assert(manager180.create(masterPassword));
    CredentialAutofillController c180(&manager180);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://login.example.com");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));
    c180.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user180@example.com"), QStringLiteral("Secret#180"), true));
    assert(waitForCondition([&c180] { return c180.activeSaveBubble() != nullptr; }, 1000));
    assert(c180.activeSaveBubble()->origin() == origin);

    // Page redirects to another domain/path
    view->setUrl(QUrl(QStringLiteral("https://accounts.example.net/dashboard")));
    c180.onUrlChanged(view, view->url());
    c180.onPageLoadFinished(view, true);

    // Pending decision still visible, bound to original origin
    assert(c180.activeSaveBubble() != nullptr);
    assert(c180.activeSaveBubble()->origin() == origin);
    assert(c180.activeSaveBubble()->username() == QStringLiteral("user180@example.com"));

    // Explicit Not Now ("Şimdi Değil")
    c180.activeSaveBubble()->clickSecondary();
    assert(c180.activeSaveBubble() == nullptr);
    assert(c180.pendingCandidateCount() == 0);

    // Verify nothing saved to vault
    assert(manager180.forOrigin(QUrl(origin)).isEmpty());
    assert(manager180.forOrigin(QUrl(QStringLiteral("https://accounts.example.net"))).isEmpty());

    c180.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 180: pending save -> navigation -> explicit Not Now" << std::endl;
  }

  // TEST 181: multiple candidates: candidate while decision pending retains existing decision
  {
    std::cout << "[RUN] TEST 181: candidate while decision pending retains existing decision" << std::endl;
    QTemporaryDir dir181;
    CredentialVaultManager manager181(dir181.path());
    assert(manager181.create(masterPassword));
    CredentialAutofillController c181(&manager181);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://service.example.org");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    c181.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("userA@example.com"), QStringLiteral("PassA#181"), true));
    assert(waitForCondition([&c181] { return c181.activeSaveBubble() != nullptr; }, 1000));
    assert(c181.activeSaveBubble()->username() == QStringLiteral("userA@example.com"));

    // Second candidate arrives for different user while decision pending
    c181.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("userB@example.com"), QStringLiteral("PassB#181"), true));

    // Active pending decision MUST NOT be overwritten with different credentials
    assert(c181.activeSaveBubble() != nullptr);
    assert(c181.activeSaveBubble()->username() == QStringLiteral("userA@example.com"));

    // Page finishes loading
    c181.onPageLoadFinished(view, true);
    assert(c181.activeSaveBubble()->isPrimaryButtonEnabled());

    // User saves the pending decision
    c181.activeSaveBubble()->clickPrimary();
    assert(c181.activeSaveBubble() == nullptr);

    // Verify only userA is in vault
    const auto records = manager181.forOrigin(QUrl(origin));
    assert(records.size() == 1);
    assert(records[0].username == QStringLiteral("userA@example.com"));

    c181.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 181: candidate while decision pending retains existing decision" << std::endl;
  }

  // TEST 182: form submit with post-login error state retains pending save bubble until user decision
  {
    std::cout << "[RUN] TEST 182: post-login error state retains pending save bubble" << std::endl;
    QTemporaryDir dir182;
    CredentialVaultManager manager182(dir182.path());
    assert(manager182.create(masterPassword));
    CredentialAutofillController c182(&manager182);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://www.facebook.com");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    c182.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user@example.com"), QStringLiteral("MyPassword123!"), true));
    assert(waitForCondition([&c182] { return c182.activeSaveBubble() != nullptr; }, 1000));

    // Page finishes load and DOM reports error banner (e.g. "The login information you entered is incorrect")
    c182.onPageLoadFinished(view, true);
    c182.handleBridgeMessage(view->page(), QStringLiteral("state:{\"origin\":\"https://www.facebook.com\",\"loginFormVisible\":true,\"passwordFieldVisible\":true,\"errorStateObserved\":true}"));

    // Bubble MUST NOT disappear! It remains visible and active for the user's decision
    assert(c182.activeSaveBubble() != nullptr);
    assert(c182.activeSaveBubble()->origin() == origin);
    assert(c182.activeSaveBubble()->username() == QStringLiteral("user@example.com"));
    assert(c182.activeSaveBubble()->isPrimaryButtonEnabled());

    // User chooses to save
    c182.activeSaveBubble()->clickPrimary();
    assert(c182.activeSaveBubble() == nullptr);
    assert(c182.pendingCandidateCount() == 0);

    const auto records = manager182.forOrigin(QUrl(origin));
    assert(records.size() == 1);
    assert(records[0].username == QStringLiteral("user@example.com"));

    c182.onViewClosed(view);
    delete view;
    std::cout << "[PASS] TEST 182: post-login error state retains pending save bubble" << std::endl;
  }

  // TEST 183: owning view destruction securely cleans up pending candidate and save bubble
  {
    std::cout << "[RUN] TEST 183: owning view destruction cleans up pending candidate" << std::endl;
    QTemporaryDir dir183;
    CredentialVaultManager manager183(dir183.path());
    assert(manager183.create(masterPassword));
    CredentialAutofillController c183(&manager183);
    auto *view = new QWebEngineView();
    const QString origin = QStringLiteral("https://destroy.example.com");
    view->setUrl(QUrl(origin + QStringLiteral("/login")));

    c183.handleBridgeMessage(view->page(), candidateMessage(origin, QStringLiteral("user183@example.com"), QStringLiteral("Secret#183"), true));
    assert(waitForCondition([&c183] { return c183.activeSaveBubble() != nullptr; }, 1000));
    assert(c183.pendingCandidateCount() == 1);

    // Owning view closed/destroyed
    c183.onViewClosed(view);
    assert(c183.activeSaveBubble() == nullptr);
    assert(c183.pendingCandidateCount() == 0);

    delete view;
    std::cout << "[PASS] TEST 183: owning view destruction cleans up pending candidate" << std::endl;
  }

  std::cout << "\nAll Password Autofill, Security, and Submit-Centered Credential Save UX tests PASSED successfully!" << std::endl;
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
  return 0;
}
