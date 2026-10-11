#include "engine/webkit/webkit_private_runtime.h"

#include <QFile>
#include <QFileInfo>

#include <dlfcn.h>
#include <link.h>

// WebKitGTK's headers use `signals` as an identifier; Qt headers come first.
#undef signals
#include <webkit2/webkit2.h>

namespace dalinira::engine {

namespace {

// Resolved path of the shared object that contains `symbol`.
QString mappedObjectOf(const void *symbol) {
  Dl_info info{};
  if (dladdr(symbol, &info) == 0 || !info.dli_fname) return {};
  const QString path = QString::fromLocal8Bit(info.dli_fname);
  const QString resolved = QFileInfo(path).canonicalFilePath();
  return resolved.isEmpty() ? path : resolved;
}

// Path the dynamic loader loaded the shared object `soname` from. Preferred
// over dladdr() on a function: in an executable that is not position
// independent (e.g. Fedora's default CMake build), the address of a library
// function is the executable's own PLT entry, so dladdr() names the browser.
QString loadedObjectPath(const char *soname) {
  void *handle = dlopen(soname, RTLD_LAZY | RTLD_NOLOAD);
  if (!handle) return {};
  QString path;
  struct link_map *map = nullptr;
  if (dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name && *map->l_name)
    path = QString::fromLocal8Bit(map->l_name);
  dlclose(handle);
  if (path.isEmpty()) return {};
  const QString resolved = QFileInfo(path).canonicalFilePath();
  return resolved.isEmpty() ? path : resolved;
}

bool isInside(const QString &path, const QString &directory) {
  const QString root = QFileInfo(directory).canonicalFilePath();
  return !root.isEmpty() && path.startsWith(root + QLatin1Char('/'));
}

}  // namespace

WebKitPrivateRuntime::Status WebKitPrivateRuntime::probe() {
  Status status;
  status.webkitLibrary = loadedObjectPath("libwebkit2gtk-4.1.so.0");
  if (status.webkitLibrary.isEmpty())
    status.webkitLibrary = mappedObjectOf(reinterpret_cast<const void *>(&webkit_get_major_version));
  status.jscLibrary = loadedObjectPath("libjavascriptcoregtk-4.1.so.0");
  if (status.jscLibrary.isEmpty())
    status.jscLibrary = mappedObjectOf(reinterpret_cast<const void *>(&jsc_get_major_version));
  status.version = QStringLiteral("%1.%2.%3")
                       .arg(webkit_get_major_version())
                       .arg(webkit_get_minor_version())
                       .arg(webkit_get_micro_version());
#if defined(DALINIRA_WEBKIT_PRIVATE_RUNTIME_PREFIX)
  status.expected = true;
  status.prefix = QStringLiteral(DALINIRA_WEBKIT_PRIVATE_RUNTIME_PREFIX);
  const QString libDir = status.prefix + QStringLiteral("/lib");
  if (!isInside(status.webkitLibrary, libDir)) {
    status.problems << QStringLiteral("libwebkit2gtk is not the private copy: %1").arg(status.webkitLibrary);
  }
  if (!isInside(status.jscLibrary, libDir)) {
    status.problems << QStringLiteral("libjavascriptcoregtk is not the private copy: %1").arg(status.jscLibrary);
  }
  // Package SDKs use libexec; the preserved development runtime was built
  // with CMAKE_INSTALL_LIBEXECDIR=lib. Both are private patched runtimes.
#if defined(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME)
  const QString helperDir = status.prefix + QStringLiteral("/lib/webkit2gtk-4.1");
#else
  const QString helperDir = status.prefix + QStringLiteral("/libexec/webkit2gtk-4.1");
#endif
  const QStringList files = {
      helperDir + QStringLiteral("/WebKitWebProcess"),
      helperDir + QStringLiteral("/WebKitNetworkProcess"),
      status.prefix + QStringLiteral("/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so"),
  };
  for (const QString &file : files) {
    if (!QFileInfo(file).isFile()) status.problems << QStringLiteral("missing %1").arg(file);
  }
  QFile buildInfo(status.prefix + QStringLiteral("/BUILD-INFO"));
  if (buildInfo.open(QIODevice::ReadOnly))
    status.buildInfo = QString::fromUtf8(buildInfo.readAll()).trimmed();
#if !defined(DALINIRA_WEBKIT_DEVELOPMENT_RUNTIME)
  else
    status.problems << QStringLiteral("missing %1").arg(buildInfo.fileName());
#endif
#endif
  return status;
}

QString WebKitPrivateRuntime::describe(const Status &status) {
  QStringList lines;
  lines << QStringLiteral("WebKitGTK %1").arg(status.version)
        << QStringLiteral("libwebkit2gtk: %1").arg(status.webkitLibrary)
        << QStringLiteral("libjavascriptcoregtk: %1").arg(status.jscLibrary);
  if (!status.expected) {
#if defined(DALINIRA_SYSTEM_WEBKIT_TEST_BUILD)
    lines << QStringLiteral("WebKit runtime: system, unpatched (TEST-ONLY build, not for release)");
#else
    lines << QStringLiteral("WebKit runtime: system (this build has no private runtime)");
#endif
    return lines.join(QLatin1Char('\n'));
  }
  lines << QStringLiteral("private runtime prefix: %1").arg(status.prefix);
  if (!status.buildInfo.isEmpty()) lines << status.buildInfo;
  for (const QString &problem : status.problems) lines << QStringLiteral("problem: %1").arg(problem);
  lines << (status.usable() ? QStringLiteral("WebKit runtime: private (patched), in use")
                            : QStringLiteral("WebKit runtime: BROKEN, the WebKit engine is disabled"));
  return lines.join(QLatin1Char('\n'));
}

}  // namespace dalinira::engine
