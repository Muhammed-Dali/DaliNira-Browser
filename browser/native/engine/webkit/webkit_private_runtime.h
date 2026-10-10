#pragma once

#include <QString>
#include <QStringList>

namespace dalinira::engine {

// DaliNira's private WebKitGTK runtime (packaging/webkit-runtime/SOURCE.md).
//
// Release packages build against a patched WebKitGTK installed under
// lib/dalinira-browser/webkit and reach it through DT_RPATH, so the system's
// WebKitGTK is never loaded. Such a build defines
// DALINIRA_WEBKIT_PRIVATE_RUNTIME_PREFIX. The normal build can point at the
// preserved build-webkit-runtime/prefix development copy. Only a test-only
// build (DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS) uses the system WebKitGTK.
class WebKitPrivateRuntime {
 public:
  struct Status {
    bool expected = false;  // Built for the private runtime.
    QString prefix;         // Its install prefix, when expected.
    QString webkitLibrary;  // libwebkit2gtk this process mapped (resolved path).
    QString jscLibrary;     // libjavascriptcoregtk this process mapped.
    QString version;        // WebKitGTK version reported by the mapped library.
    QString buildInfo;      // <prefix>/BUILD-INFO, when present.
    QStringList problems;   // Why the private runtime is not (fully) in use.

    bool usable() const { return problems.isEmpty(); }
  };

  // Inspects the libraries the dynamic loader mapped into this process and
  // the private runtime's files. Needs no display.
  static Status probe();
  // Multi-line report for `dalinira-browser --check-webkit-runtime`.
  static QString describe(const Status &status);
};

} // namespace dalinira::engine
