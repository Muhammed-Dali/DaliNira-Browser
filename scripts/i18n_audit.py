#!/usr/bin/env python3
"""Deterministic localization audit for DaliNira (Chromium + WebKit share it).

Fails when:
  * a catalog is not valid JSON, contains duplicate keys, or empty values;
  * tr/en/ar/fr do not have exactly the same key set;
  * placeholders (%1..%99, %s, {name}), leading/trailing whitespace, line
    breaks or markup tags differ between a translation and the Turkish source;
  * code looks up a key that is missing, or passes a hardcoded fallback;
  * a catalog key is never referenced (obsolete);
  * a user-visible Turkish or English literal remains in native UI code or in
    an internal HTML/JS template outside the reviewed allowlist;
  * the application layout could be switched to right-to-left.

Run: python3 scripts/i18n_audit.py [--verbose]
"""

from __future__ import annotations

import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG_DIR = ROOT / "i18n" / "translations"
ALLOWLIST = ROOT / "scripts" / "i18n_allowlist.json"
LANGUAGES = ("tr", "en", "ar", "fr")
SOURCE_DIR = ROOT / "browser" / "native"

PLACEHOLDER = re.compile(r"%\d{1,2}(?!\d)|%s|\{[A-Za-z_][A-Za-z0-9_]*\}")
TAG = re.compile(r"</?[a-zA-Z][a-zA-Z0-9]*")
ANY_KEY_STRING = re.compile(r'"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)"')
KEY_CALL = re.compile(r'I18n::(?:text|tr)\(\s*QStringLiteral\("([^"]+)"\)')
TEMPLATE_TOKEN = re.compile(r"\{\{i18n(?:-js|-html)?:([a-z0-9_]+\.[A-Za-z0-9_.]+)\}\}")
FALLBACK_CALL = re.compile(
    r'I18n::(?:text|tr)\(\s*QStringLiteral\("[^"]+"\)\s*,\s*(?:QStringLiteral|QString::fromUtf8)\(')
TURKISH_CHARS = re.compile(r"[çğıöşüÇĞİÖŞÜ]")
LOG_CALL = re.compile(
    r"\b(q(?:C)?(?:Debug|Info|Warning|Critical|Fatal)|std::(?:cerr|cout)|fprintf|printf)\b")
UI_CALL = re.compile(
    r"setText|setToolTip|addAction|QMessageBox|setWindowTitle|setPlaceholderText|"
    r"setAccessibleName|setAccessibleDescription|QLabel\(|QPushButton\(|QCheckBox\(|"
    r"QRadioButton\(|addItem|addItems|QAction\(|setStatusTip|showMessage|setTitle|addMenu|"
    r"QInputDialog|setHeaderLabels|setLabelText|setSpecialValueText|setPrefix|setSuffix|"
    r"GlowToggleSwitch\(|QGroupBox\(")
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
WRAPPED = re.compile(r'\b(QStringLiteral|QString::fromUtf8|QLatin1String|QString|tr)\s*\(\s*('
                     + LITERAL + r'(?:\s*' + LITERAL + r')*)\s*\)')
BARE_GROUP = re.compile(LITERAL + r'(?:\s*' + LITERAL + r')*')
RAW_STRING = re.compile(r'R"([^()\s]{0,16})\((.*?)\)\1"', re.S)

# Keys composed at runtime (prefix + id); they count as referenced.
DYNAMIC_PREFIXES = ("language.name.", "eq.group_", "newtab.search_placeholder_")


class Audit:
    def __init__(self, verbose: bool) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.verbose = verbose

    def fail(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)


def load_catalog(audit: Audit, language: str) -> dict[str, str] | None:
    path = CATALOG_DIR / f"{language}.json"
    rel = path.relative_to(ROOT)

    def no_duplicates(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                audit.fail(f"{rel}: duplicate key {key}")
            seen[key] = value
        return seen

    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except (OSError, json.JSONDecodeError) as exc:
        audit.fail(f"{rel}: cannot read catalog: {exc}")
        return None
    if not isinstance(data, dict):
        audit.fail(f"{rel}: catalog root must be an object")
        return None
    for key, value in data.items():
        if not isinstance(value, str) or not value.strip():
            audit.fail(f"{language}: empty/non-string translation for {key}")
        if key.startswith("ui.source."):
            audit.fail(f"{language}: legacy hash key {key}; use a semantic key")
    return data


def edge_whitespace(text: str) -> tuple[str, str]:
    return (re.match(r"^\s*", text).group(0), re.search(r"\s*$", text).group(0))


def check_catalogs(audit: Audit, catalogs: dict[str, dict[str, str]]) -> None:
    baseline = set(catalogs["tr"])
    for language in LANGUAGES[1:]:
        keys = set(catalogs[language])
        for key in sorted(baseline - keys):
            audit.fail(f"{language}: missing key {key}")
        for key in sorted(keys - baseline):
            audit.fail(f"{language}: key {key} exists only in {language}")

    for key in sorted(baseline):
        source = catalogs["tr"][key]
        expected = sorted(PLACEHOLDER.findall(source))
        for language in LANGUAGES[1:]:
            value = catalogs[language].get(key)
            if value is None:
                continue
            actual = sorted(PLACEHOLDER.findall(value))
            if actual != expected:
                audit.fail(f"{language}: placeholder mismatch for {key}: expected {expected}, got {actual}")
            if edge_whitespace(value) != edge_whitespace(source):
                audit.fail(f"{language}: leading/trailing whitespace differs from tr for {key}")
            if value.count("\n") != source.count("\n"):
                audit.fail(f"{language}: line-break count differs from tr for {key}")
            if sorted(TAG.findall(value)) != sorted(TAG.findall(source)):
                audit.fail(f"{language}: markup tags differ from tr for {key}")

    # Identical text in one language mapped to keys whose translations differ
    # elsewhere makes live widget retranslation ambiguous. Reported, not fatal:
    # the remaining cases are synonyms/capitalisation variants.
    ambiguous = 0
    for language in LANGUAGES:
        groups = collections.defaultdict(list)
        for key, value in catalogs[language].items():
            groups[value].append(key)
        for value, keys in groups.items():
            if len(keys) > 1 and any(len({catalogs[other].get(k) for k in keys}) > 1 for other in LANGUAGES):
                ambiguous += 1
                if audit.verbose:
                    audit.warn(f"{language}: {value!r} shared by {keys}")
    if ambiguous and not audit.verbose:
        audit.warn(f"{ambiguous} shared values map to keys that differ in another language (use --verbose)")


def mask_comments(text: str) -> str:
    out = list(text)
    for m in RAW_STRING.finditer(text):
        for i in range(m.start(), m.end()):
            if out[i] != "\n":
                out[i] = " "
    masked = "".join(out)
    out = list(masked)
    i, n, in_string = 0, len(masked), False
    while i < n:
        c = masked[i]
        if in_string:
            if c == "\\":
                i += 2
                continue
            if c in "\"\n":
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
        elif c == "'" and 0 < masked.find("'", i + 1) - i <= 4:
            i = masked.find("'", i + 1) + 1
            continue
        elif masked.startswith("//", i):
            j = masked.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        elif masked.startswith("/*", i):
            j = masked.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def decode(group: str) -> str:
    out = []
    for part in re.findall(LITERAL, group):
        body = part[1:-1]
        body = body.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")
        out.append(body)
    return "".join(out)


def turkish_vocabulary(catalogs: dict[str, dict[str, str]]) -> set[str]:
    """Words that occur in Turkish UI text but never in the English catalog."""
    def words(text: str) -> set[str]:
        return {w.lower() for w in re.findall(r"[A-Za-zÇĞİÖŞÜçğıöşü]{3,}", text)}
    tr_words: set[str] = set()
    en_words: set[str] = set()
    for value in catalogs["tr"].values():
        tr_words |= words(value)
    for language in ("en", "fr"):
        for value in catalogs[language].values():
            en_words |= words(value)
    return {w for w in tr_words - en_words if not TURKISH_CHARS.search(w)}


def load_allowlist(audit: Audit) -> list[dict]:
    try:
        entries = json.loads(ALLOWLIST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        audit.fail(f"{ALLOWLIST.relative_to(ROOT)}: {exc}")
        return []
    for entry in entries:
        if not entry.get("reason"):
            audit.fail(f"allowlist entry without reason: {entry}")
        entry["_re"] = re.compile(entry.get("pattern", ".*"), re.S)
        entry["_used"] = False
    return entries


def allowed(entries: list[dict], rel: str, text: str, context: str) -> bool:
    for entry in entries:
        if not rel.endswith(entry["file"]):
            continue
        if entry["_re"].search(text) or ("context" in entry and re.search(entry["context"], context)):
            entry["_used"] = True
            return True
    return False


NON_UI_CONTEXT = re.compile(
    r"^\s*#\s*include|setObjectName|setProperty|objectName|QSettings|setValue\(|\.value\(|"
    r"QIcon\(|QUrl\(|QRegularExpression|findChild|scriptName|setWorldId")


def is_identifier(text: str) -> bool:
    """Paths, resource names, CSS/HTML snippets and machine identifiers."""
    if " " not in text.strip() and re.search(r"[/:._<>\-]", text):
        return True
    return text.startswith(":/") or text.lstrip().startswith("<") and text.rstrip().endswith(">")


def looks_turkish(text: str, vocab: set[str]) -> bool:
    if is_identifier(text):
        return False
    if TURKISH_CHARS.search(text):
        return True
    for word in re.findall(r"[A-Za-z]{3,}", text):
        if word.lower() in vocab:
            return True
    return False


def looks_english_ui(text: str) -> bool:
    if re.search(r"[/_=#{}<>\\]|::|\.\w+\(|^[a-z]+(-[a-z]+)+$", text):
        return False
    return bool(re.fullmatch(r"[A-Z][a-z']+(?: [A-Za-z'’,.…()&/-]+)+[.!?…:]?", text))


def scan_sources(audit: Audit, catalogs, entries) -> tuple[set[str], set[str], int]:
    vocab = turkish_vocabulary(catalogs)
    english_values = set(catalogs["en"].values())
    referenced: set[str] = set()
    looked_up: set[str] = set()
    literals = 0
    for path in sorted(SOURCE_DIR.rglob("*")):
        if path.suffix not in (".cpp", ".h") or "/tests/" in path.as_posix() or "/tools/" in path.as_posix():
            continue
        rel = str(path.relative_to(ROOT))
        raw = path.read_text(encoding="utf-8")
        lines = raw.split("\n")
        referenced.update(ANY_KEY_STRING.findall(raw))
        referenced.update(TEMPLATE_TOKEN.findall(raw))
        looked_up.update(KEY_CALL.findall(raw))
        looked_up.update(TEMPLATE_TOKEN.findall(raw))

        for m in FALLBACK_CALL.finditer(raw):
            line = raw.count("\n", 0, m.start()) + 1
            audit.fail(f"{rel}:{line}: hardcoded fallback passed to I18n lookup")

        masked = mask_comments(raw)
        spans = []
        for m in WRAPPED.finditer(masked):
            spans.append((m.start(), m.end(), m.group(1), raw[m.start(2):m.end(2)]))
        taken = [(a, b) for a, b, *_ in spans]
        for m in BARE_GROUP.finditer(masked):
            if any(a <= m.start() < b for a, b in taken):
                continue
            spans.append((m.start(), m.end(), "", raw[m.start():m.end()]))
        for start, _end, wrapper, group in spans:
            text = decode(group)
            if not text.strip():
                continue
            line_no = raw.count("\n", 0, start) + 1
            context = lines[line_no - 1]
            if LOG_CALL.search(context) or NON_UI_CONTEXT.search(context):
                continue
            key_call = re.search(r"I18n::(?:text|tr)\(\s*$", masked[max(0, start - 40):start])
            if key_call or re.fullmatch(r"[a-z][a-z0-9_]*(\.[a-z0-9_]+)+", text):
                continue
            problem = None
            if looks_turkish(text, vocab):
                problem = "Turkish UI literal"
            elif UI_CALL.search(context) and (text in english_values or looks_english_ui(text)):
                problem = "English UI literal"
            if problem and not allowed(entries, rel, text, context):
                literals += 1
                audit.fail(f"{rel}:{line_no}: {problem} not in catalog: {text[:90]!r}")

        # Internal HTML/JS templates: visible text must be tokenized.
        for m in RAW_STRING.finditer(raw):
            body = m.group(2)
            if "<html" not in body and "<body" not in body and "<p" not in body:
                continue
            start_line = raw.count("\n", 0, m.start()) + 1
            visible = re.sub(r"<style>.*?</style>", "", body, flags=re.S)
            for t in re.finditer(r">([^<>{}]*[A-Za-zÇĞİÖŞÜçğıöşü][^<>{}]*)<", visible):
                chunk = t.group(1).strip()
                if chunk and looks_turkish(chunk, vocab) and not allowed(entries, rel, chunk, ""):
                    audit.fail(f"{rel}:{start_line}: untranslated text in HTML template: {chunk[:80]!r}")
            for t in re.finditer(r'\b(?:title|aria-label|placeholder|alt)="([^"{}]+)"', visible):
                if looks_turkish(t.group(1), vocab) and not allowed(entries, rel, t.group(1), ""):
                    audit.fail(f"{rel}:{start_line}: untranslated attribute in HTML template: {t.group(1)[:80]!r}")
            for t in re.finditer(r"'([^'\n]*[çğıöşüÇĞİÖŞÜ][^'\n]*)'", visible):
                if not allowed(entries, rel, t.group(1), ""):
                    audit.fail(f"{rel}:{start_line}: untranslated JS string in template: {t.group(1)[:80]!r}")
            if re.search(r'<html(?![^>]*dir="ltr")', body):
                audit.fail(f"{rel}:{start_line}: internal page <html> must declare dir=\"ltr\"")
            if 'lang="tr"' in body:
                audit.fail(f"{rel}:{start_line}: internal page hardcodes lang=\"tr\"")
    return referenced, looked_up, literals


def main() -> int:
    audit = Audit("--verbose" in sys.argv)
    catalogs = {}
    for language in LANGUAGES:
        data = load_catalog(audit, language)
        if data is not None:
            catalogs[language] = data
    if len(catalogs) != len(LANGUAGES):
        return report(audit, 0)

    check_catalogs(audit, catalogs)
    entries = load_allowlist(audit)
    referenced, looked_up, literals = scan_sources(audit, catalogs, entries)

    for key in sorted(looked_up):
        if key not in catalogs["tr"] and not key.startswith(DYNAMIC_PREFIXES) and not key.endswith((".", "_")):
            audit.fail(f"code looks up missing key {key}")
    # A looked-up literal ending in "." or "_" is a prefix completed at runtime
    # (e.g. "onboarding.search_privacy.provider_" + engine): its keys are used.
    runtime_prefixes = DYNAMIC_PREFIXES + tuple(k for k in looked_up if k.endswith((".", "_")))
    for key in sorted(catalogs["tr"]):
        if key not in referenced and not key.startswith(runtime_prefixes) and key != "app.name":
            audit.fail(f"obsolete catalog key {key} (not referenced by native code)")
    for entry in entries:
        if not entry["_used"]:
            audit.warn(f"unused allowlist entry: {entry['file']} {entry.get('pattern', entry.get('context'))}")

    language_manager = (SOURCE_DIR / "i18n" / "language_manager.cpp").read_text(encoding="utf-8")
    if "setLayoutDirection(Qt::LeftToRight)" not in language_manager:
        audit.fail("LanguageManager must pin the application layout to Qt::LeftToRight")
    for path in SOURCE_DIR.rglob("*.cpp"):
        if "/tests/" in path.as_posix():
            continue
        text = path.read_text(encoding="utf-8")
        if re.search(r"setLayoutDirection\(\s*Qt::RightToLeft", text) or 'dir="rtl"' in text \
                or "dir='rtl'" in text:
            audit.fail(f"{path.relative_to(ROOT)}: browser chrome must never switch to right-to-left")
    return report(audit, len(catalogs["tr"]))


def report(audit: Audit, count: int) -> int:
    for warning in audit.warnings:
        print(f"warning: {warning}")
    if audit.errors:
        print(f"Localization audit FAILED ({len(audit.errors)} problems):", file=sys.stderr)
        for error in audit.errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"Localization audit passed: {count} keys, tr/en/ar/fr in exact parity, "
          "placeholders consistent, no hardcoded UI literals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
