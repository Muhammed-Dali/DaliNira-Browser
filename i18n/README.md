# DaliNira Browser i18n

This directory holds the single localization catalog shared by both rendering
engines (Chromium and WebKit). There are no engine-specific translations.

```
i18n/
├── languages.json         # Supported locales, native names, text direction metadata
├── translations/
│   ├── tr.json            # Türkçe (source wording)
│   ├── en.json            # English (fallback catalog)
│   ├── ar.json            # العربية
│   └── fr.json            # Français
└── README.md
```

## Rules

- **Semantic keys.** Every user-visible string is looked up by a stable key
  such as `settings.performance.tab_sleep_card`, never by its text:
  `I18n::text(QStringLiteral("settings.performance.tab_sleep_card"))`.
- **Exact parity.** `tr`, `en`, `ar` and `fr` contain exactly the same keys.
  A missing key falls back to English, never to hardcoded Turkish, and code
  must not pass literal fallbacks to `I18n::text`.
- **Placeholders are preserved exactly** (`%1`, `%2`, `%s`, `{name}`), as are
  leading/trailing spaces, line breaks and markup tags in `*_html` keys.
- **Proper names stay as they are**: DaliNira, Chromium, WebKit, Pulse,
  PipeWire, YouTube, DeepL, LibreTranslate, protocol names and URLs.
- **Browser chrome is always left-to-right.** Arabic text is shaped naturally
  inside each widget, but the application layout direction stays
  `Qt::LeftToRight` and internal pages declare `dir="ltr"`: toolbar, tab strip,
  menus, Settings and New Tab never mirror. `direction` in `languages.json`
  is text metadata only.

## Runtime

- `LanguageManager` (`browser/native/i18n/language_manager.*`) loads the
  catalogs (embedded as Qt resources `:/i18n/...`), resolves the preference
  (`i18n/language` in QSettings, or the system language) and emits
  `languageChanged()` on a switch. It initializes lazily, so code that runs
  before `main()` configures it still gets real catalogs.
- `DALINIRA_UI_LANGUAGE=<code>` replaces the *system* language (tests and
  controlled GUI validation); an explicit user preference still wins.
- Live switching: `installApplicationRetranslator()` refreshes every open
  window. `retranslateWidgetTree()` maps each visible string back to its key
  (`LanguageManager::keyForText`) and re-applies it in the new language;
  composed strings (with arguments) are refreshed by their owners' own
  `languageChanged` handlers. Widgets that show user data can opt out with the
  `dalinira_i18n_skip` property.
- Values that may outlive a switch (tab titles, stored download states) are
  compared with `I18n::isTranslationOf(key, value)`, which matches every
  language.
- Internal HTML pages (New Tab, private tab, block pages, local media player)
  use template tokens rendered by `I18n::localizeHtml`: `{{i18n:key}}` (HTML),
  `{{i18n-js:key}}` (JavaScript string literal), `{{i18n-html:key}}` (trusted
  catalog markup) and `{{i18n-lang}}` (active language code).

## Checks

```
python3 scripts/i18n_audit.py            # parity, placeholders, hardcoded UI text, RTL
python3 scripts/i18n_audit_selftest.py   # proves the audit fails on each defect class
```

The audit fails on missing/extra/empty keys, duplicate JSON keys, placeholder,
whitespace, line-break or markup drift, literal fallbacks, lookups of missing
keys, obsolete keys, Turkish or English UI literals left in native code or in
internal HTML templates, and any right-to-left application layout. Legitimate
non-UI strings (language-detection word lists, protocol identifiers, product
names) are listed with a reason in `scripts/i18n_allowlist.json`.

Both scripts run in CTest (`i18n-audit`, `i18n-audit-selftest`), together with
`language-manager-test` and `browser-localization-ui` (every Settings category
in all four languages with live switching, New Tab, Arabic LTR).
