/* DaliNira Gecko Host default preferences (M0 prototype).
 *
 * Loaded from $app/defaults/preferences/ (modules/libpref/Preferences.cpp).
 * Firefox's browser/defaults/preferences/firefox.js is NOT loaded for this
 * application, so every security default it carries must be restated here.
 * See artifacts/pref-audit.md for the full comparison.
 */

// --- Application shell -----------------------------------------------------
// Opened by toolkit/components/DefaultCLH.sys.mjs at startup.
pref("toolkit.defaultChromeURI", "chrome://dng/content/host.xhtml");
pref("toolkit.defaultChromeFeatures", "chrome,dialog=no,all");
pref("toolkit.singletonWindowType", "dalinira:geckohost");

// --- Security: Linux sandbox (verbatim from firefox.js, Linux block) --------
// The GRE default is 0 (modules/libpref/init/StaticPrefList.yaml) and Linux
// enforces no minimum (security/sandbox/common/SandboxSettings.cpp), so
// without these lines content processes would run unsandboxed.
pref("security.sandbox.content.level", 6);
pref("security.sandbox.content.write_path_whitelist", "");
pref("security.sandbox.content.read_path_whitelist", "");
pref("security.sandbox.content.syscall_whitelist", "");
pref("security.sandbox.socket.process.level", 2);

// --- Security: other firefox.js defaults restated (see pref-audit.md) -------
pref("security.allow_parent_unrestricted_js_loads", false);
pref("browser.tabs.remote.separatePrivilegedContentProcess", true);
pref("browser.tabs.remote.separatePrivilegedMozillaWebContentProcess", true);
pref("extensions.webextensions.remote", true);
pref("dom.disable_open_during_load", true);
pref("security.mixed_content.block_active_content", true);
pref("network.cookie.cookieBehavior", 5);
pref("network.cookie.cookieBehavior.pbmode", 5);
pref("xpinstall.signatures.required", true);
pref("extensions.langpacks.signatures.required", true);
pref("extensions.experiments.enabled", false);
pref("extensions.autoDisableScopes", 15);
pref("privacy.authPromptSpoofingProtection", true);
pref("privacy.trackingprotection.cryptomining.enabled", true);
pref("privacy.trackingprotection.fingerprinting.enabled", true);
pref("privacy.trackingprotection.harmfuladdon.enabled", true);
pref("privacy.globalprivacycontrol.pbmode.enabled", true);

// --- Prototype: no data upload, no background updates ----------------------
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.enabled", false);
// app.update.enabled is ignored by this Gecko. DisableAppUpdate is applied
// by the stock policy engine using our application policy modules; the
// launcher supplies its profile-local alternatePath before engine startup.

// --- Prototype diagnostics (logging only; no security effect) ---------------
pref("browser.dom.window.dump.enabled", true);
pref("devtools.console.stdout.chrome", true);

// Route content window requests into the host's native browserDOMWindow tab
// implementation. Gecko still enforces its popup/user-activation policy.
pref("browser.link.open_newwindow", 3);
pref("browser.link.open_newwindow.restriction", 0);
