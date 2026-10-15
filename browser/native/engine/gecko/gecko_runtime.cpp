#include "engine/gecko/gecko_runtime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <sys/stat.h>
#include <unistd.h>

namespace dalinira::engine {

namespace {

bool ownedByUserOrRoot(const struct stat &st) { return st.st_uid == ::getuid() || st.st_uid == 0; }

// The directory and every directory above it can be changed only by this
// user or root. A root-owned sticky directory (/tmp) protects its entries.
bool trustedDirectoryChain(const QString &canonicalDirectory, QString *problem) {
  QString path = canonicalDirectory;
  while (true) {
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      *problem = QStringLiteral("%1 is not a directory").arg(path);
      return false;
    }
    const bool othersCanWrite = (st.st_mode & 0022) != 0;
    const bool rootSticky = st.st_uid == 0 && (st.st_mode & S_ISVTX);
    if (!ownedByUserOrRoot(st) || (othersCanWrite && !rootSticky)) {
      *problem = QStringLiteral("%1 can be changed by other users").arg(path);
      return false;
    }
    if (path == QLatin1String("/")) return true;
    path = QFileInfo(path).path();
  }
}

// `path` (inside the canonical `directory`) is a regular file of this user or
// root that nobody else can write, and does not lead outside `directory`.
bool trustedFile(const QString &directory, const QString &path, bool executable, QString *problem) {
  const QString canonical = QFileInfo(path).canonicalFilePath();
  struct stat st {};
  if (canonical.isEmpty() || ::stat(QFile::encodeName(canonical).constData(), &st) != 0) {
    *problem = QStringLiteral("%1 is missing").arg(path);
    return false;
  }
  if (!canonical.startsWith(directory + QLatin1Char('/'))) {
    *problem = QStringLiteral("%1 leads outside %2").arg(path, directory);
    return false;
  }
  if (!S_ISREG(st.st_mode) || !ownedByUserOrRoot(st) || (st.st_mode & 0022) != 0) {
    *problem = QStringLiteral("%1 is not a regular file only its owner can write").arg(path);
    return false;
  }
  if (executable && ::access(QFile::encodeName(canonical).constData(), X_OK) != 0) {
    *problem = QStringLiteral("%1 is not executable").arg(path);
    return false;
  }
  return true;
}

QString canonicalDirectory(const QString &path, const char *what, QStringList *problems) {
  if (path.isEmpty() || QDir::isRelativePath(path)) {
    problems->append(QStringLiteral("%1 is not an absolute path: '%2'").arg(QString::fromLatin1(what), path));
    return {};
  }
  const QString canonical = QFileInfo(path).canonicalFilePath();
  QString problem;
  if (canonical.isEmpty() || !trustedDirectoryChain(canonical, &problem)) {
    problems->append(QStringLiteral("%1: %2").arg(QString::fromLatin1(what),
                                                  canonical.isEmpty() ? path + QStringLiteral(" does not exist") : problem));
    return {};
  }
  return canonical;
}

// One value of an INI file, read now (QSettings caches files process-wide).
QString iniValue(const QString &path, const QString &section, const QString &key) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QString current;
  for (const QByteArray &raw : file.readAll().split('\n')) {
    const QString line = QString::fromUtf8(raw).trimmed();
    if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
      current = line.mid(1, line.size() - 2);
    } else if (current == section && line.section(QLatin1Char('='), 0, 0).trimmed() == key) {
      return line.section(QLatin1Char('='), 1).trimmed();
    }
  }
  return {};
}

int majorVersion(const QString &version) {
  bool ok = false;
  const int major = version.section(QLatin1Char('.'), 0, 0).toInt(&ok);
  return ok ? major : -1;
}

}  // namespace

GeckoValidatedRuntime GeckoRuntime::validate(const GeckoRuntimePaths &paths, QStringList *problems) {
  QStringList found;
  QString problem;
  const QString gre = canonicalDirectory(paths.greDirectory, "Gecko runtime", &found);
  const QString app = canonicalDirectory(paths.hostApplicationDirectory, "Gecko host application", &found);

  GeckoValidatedRuntime runtime;
  QString applicationName;
  QString maxVersion;
  if (!gre.isEmpty()) {
    runtime.greDirectory = gre;
    runtime.executable = gre + QStringLiteral("/firefox");
    if (!trustedFile(gre, runtime.executable, true, &problem)) found.append(problem);
    for (const char *name : {"libxul.so", "plugin-container", "platform.ini", "modules/AppConstants.sys.mjs"}) {
      if (!trustedFile(gre, gre + QLatin1Char('/') + QString::fromLatin1(name), false, &problem)) found.append(problem);
    }
    runtime.milestone = iniValue(gre + QStringLiteral("/platform.ini"), QStringLiteral("Build"), QStringLiteral("Milestone"));
    // The update-disable policy is read from the host application through
    // browser.policies.alternatePath, which only Nightly builds honour.
    QFile constants(gre + QStringLiteral("/modules/AppConstants.sys.mjs"));
    if (!constants.open(QIODevice::ReadOnly) || !constants.readAll().contains("NIGHTLY_BUILD: true,")) {
      found.append(QStringLiteral("the Gecko runtime is not a Nightly development build; packaged runtimes need "
                                  "DaliNira's dedicated Gecko build to disable application updates"));
    }
    if (QFileInfo::exists(gre + QStringLiteral("/distribution/policies.json"))) {
      found.append(QStringLiteral("%1/distribution/policies.json would take precedence over the host's update policy").arg(gre));
    }
  }
  if (!app.isEmpty()) {
    runtime.hostApplicationDirectory = app;
    runtime.applicationIni = app + QStringLiteral("/application.ini");
    runtime.updatePolicyFile = app + QStringLiteral("/distribution/policies.json");
    const QStringList hostFiles = QFileInfo::exists(app + QStringLiteral("/omni.ja"))
        ? QStringList{runtime.applicationIni, app + QStringLiteral("/omni.ja"), runtime.updatePolicyFile}
        : QStringList{runtime.applicationIni, app + QStringLiteral("/chrome.manifest"),
                      app + QStringLiteral("/chrome/content/host.js"), runtime.updatePolicyFile};
    for (const QString &file : hostFiles) {
      if (!trustedFile(app, file, false, &problem)) found.append(problem);
    }
    applicationName = iniValue(runtime.applicationIni, QStringLiteral("App"), QStringLiteral("Name"));
    maxVersion = iniValue(runtime.applicationIni, QStringLiteral("Gecko"), QStringLiteral("MaxVersion"));
    if (applicationName != QLatin1String("DaliNiraGeckoHost")) {
      found.append(QStringLiteral("%1 is not the DaliNira Gecko Host").arg(runtime.applicationIni));
    }
    QFile policy(runtime.updatePolicyFile);
    const QJsonObject expected{{QStringLiteral("policies"), QJsonObject{{QStringLiteral("DisableAppUpdate"), true}}}};
    if (!policy.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(policy.readAll()).object() != expected) {
      found.append(QStringLiteral("%1 must disable application updates and nothing else").arg(runtime.updatePolicyFile));
    }
    // System policies take precedence over the host's own.
    QStringList roots{QStringLiteral("/etc"), QStringLiteral("/app/etc")};
    if (!qEnvironmentVariableIsEmpty("MOZ_SYSTEM_CONFIG_DIR")) roots.append(qEnvironmentVariable("MOZ_SYSTEM_CONFIG_DIR"));
    for (const QString &root : std::as_const(roots)) {
      const QString system = QStringLiteral("%1/%2/policies/policies.json").arg(root, applicationName.toLower());
      if (!applicationName.isEmpty() && QFileInfo::exists(system)) {
        found.append(QStringLiteral("%1 would take precedence over the host's update policy").arg(system));
      }
    }
  }
  if (!runtime.milestone.isEmpty() && maxVersion.endsWith(QLatin1String(".*")) &&
      majorVersion(runtime.milestone) != majorVersion(maxVersion)) {
    found.append(QStringLiteral("Gecko %1 is not the version the host application supports (%2)")
                     .arg(runtime.milestone, maxVersion));
  }

  if (problems) *problems = found;
  if (!found.isEmpty()) return {};
  return runtime;
}

GeckoRuntimePaths GeckoRuntime::developmentPaths() {
  GeckoRuntimePaths paths;
  paths.greDirectory = qEnvironmentVariable("DALINIRA_GECKO_RUNTIME");
  paths.hostApplicationDirectory = qEnvironmentVariable("DALINIRA_GECKO_HOST_APP");
#if defined(DALINIRA_GECKO_DEVELOPMENT_RUNTIME)
  if (paths.greDirectory.isEmpty()) paths.greDirectory = QStringLiteral(DALINIRA_GECKO_DEVELOPMENT_RUNTIME);
#endif
#if defined(DALINIRA_GECKO_HOST_APP_DIR)
  if (paths.hostApplicationDirectory.isEmpty()) paths.hostApplicationDirectory = QStringLiteral(DALINIRA_GECKO_HOST_APP_DIR);
#endif
  return paths;
}

}  // namespace dalinira::engine
