#!/usr/bin/env python3
"""Proves scripts/i18n_audit.py fails for each class of localization defect.

Each case copies the catalogs, the native sources and the audit into a
temporary tree, injects one defect and expects a non-zero exit plus a
specific message. The unmodified copy must pass.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def make_tree(dest: Path) -> None:
    shutil.copytree(ROOT / "i18n", dest / "i18n")
    shutil.copytree(ROOT / "browser" / "native", dest / "browser" / "native",
                    ignore=shutil.ignore_patterns("tests", "tools"))
    (dest / "scripts").mkdir()
    shutil.copy2(ROOT / "scripts" / "i18n_audit.py", dest / "scripts" / "i18n_audit.py")
    shutil.copy2(ROOT / "scripts" / "i18n_allowlist.json", dest / "scripts" / "i18n_allowlist.json")


def run(tree: Path) -> tuple[int, str]:
    proc = subprocess.run([sys.executable, str(tree / "scripts" / "i18n_audit.py")],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def edit_catalog(tree: Path, language: str, change) -> None:
    path = tree / "i18n" / "translations" / f"{language}.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    change(data)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def add_key_everywhere(tree: Path, key: str) -> None:
    for language in ("tr", "en", "ar", "fr"):
        edit_catalog(tree, language, lambda d: d.update({key: "Selftest"}))


def append_source(tree: Path, relative: str, snippet: str) -> None:
    path = tree / relative
    path.write_text(path.read_text(encoding="utf-8") + "\n" + snippet + "\n", encoding="utf-8")


def main() -> int:
    cases = {
        "baseline passes": (lambda t: None, None),
        "missing key": (lambda t: edit_catalog(t, "fr", lambda d: d.pop("settings.title")),
                        "fr: missing key settings.title"),
        "extra key": (lambda t: edit_catalog(t, "ar", lambda d: d.update({"only.arabic": "x"})),
                      "exists only in ar"),
        "empty translation": (lambda t: edit_catalog(t, "en", lambda d: d.update({"settings.title": " "})),
                              "empty/non-string translation"),
        "placeholder mismatch": (
            lambda t: edit_catalog(t, "ar", lambda d: d.update({"tab.hover.memory": "استخدام الذاكرة"})),
            "placeholder mismatch for tab.hover.memory"),
        "duplicate key": (
            lambda t: (t / "i18n/translations/tr.json").write_text(
                (t / "i18n/translations/tr.json").read_text(encoding="utf-8").replace(
                    '{\n', '{\n  "settings.title": "Ayarlar",\n', 1), encoding="utf-8"),
            "duplicate key settings.title"),
        "hardcoded Turkish literal": (
            lambda t: append_source(t, "browser/native/settings/settings_page.cpp",
                                    'static void injected(QLabel *l) { l->setText(QStringLiteral("Yeni ayar görünümü")); }'),
            "Turkish UI literal not in catalog"),
        "hardcoded English UI literal": (
            lambda t: append_source(t, "browser/native/settings/settings_page.cpp",
                                    'static void injected2(QLabel *l) { l->setToolTip(QStringLiteral("Open settings")); }'),
            "English UI literal not in catalog"),
        "hardcoded fallback": (
            lambda t: append_source(t, "browser/native/settings/settings_page.cpp",
                                    'static QString injected3() { return I18n::text(QStringLiteral("settings.title"), QStringLiteral("Ayarlar")); }'),
            "hardcoded fallback"),
        "missing looked-up key": (
            lambda t: append_source(t, "browser/native/settings/settings_page.cpp",
                                    'static QString injected4() { return I18n::text(QStringLiteral("settings.not_there")); }'),
            "looks up missing key settings.not_there"),
        # A key reached only through a runtime prefix ("...provider_" + id) is
        # in use; a key nothing reaches is still obsolete.
        "runtime-prefix key in use": (
            lambda t: (add_key_everywhere(t, "selftest.provider_alpha"),
                       append_source(t, "browser/native/settings/settings_page.cpp",
                                     'static QString injected6(const QString &id) '
                                     '{ return I18n::text(QStringLiteral("selftest.provider_") + id); }')),
            None),
        "unreferenced key": (lambda t: add_key_everywhere(t, "selftest.unused_key"),
                             "obsolete catalog key selftest.unused_key"),
        "right-to-left chrome": (
            lambda t: append_source(t, "browser/native/settings/settings_page.cpp",
                                    'static void injected5() { qApp->setLayoutDirection(Qt::RightToLeft); }'),
            "never switch to right-to-left"),
    }
    failed = 0
    for name, (mutate, expected) in cases.items():
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            make_tree(tree)
            mutate(tree)
            code, output = run(tree)
        ok = (code == 0) if expected is None else (code != 0 and expected in output)
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
        if not ok:
            failed += 1
            print(output[-2000:])
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
