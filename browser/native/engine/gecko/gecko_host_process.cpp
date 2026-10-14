#include "engine/gecko/gecko_host_process.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSet>
#include <QThread>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcGeckoHost, "dalinira.gecko.host")

namespace dalinira::engine {

namespace {

struct ProcStat {
  qint64 ppid = 0;
  quint64 startTime = 0;
  bool zombie = false;
  bool valid = false;
};

ProcStat readStat(qint64 pid) {
  ProcStat result;
  QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!file.open(QIODevice::ReadOnly)) return result;
  const QByteArray data = file.readAll();
  const qsizetype close = data.lastIndexOf(')');
  if (close < 0) return result;
  const QList<QByteArray> fields = data.mid(close + 2).split(' ');
  if (fields.size() < 20) return result;
  result.zombie = fields[0] == "Z";
  result.ppid = fields[1].toLongLong();
  result.startTime = fields[19].toULongLong();
  result.valid = true;
  return result;
}

bool stillSameProcess(const GeckoHostProcess::ProcessId &id) {
  const ProcStat stat = readStat(id.pid);
  return stat.valid && !stat.zombie && stat.startTime == id.startTime;
}

bool makePrivateDirectory(const QString &path) {
  return ::mkdir(QFile::encodeName(path).constData(), 0700) == 0;
}

// A profile kept across launches: created private, or an existing private
// directory of this user (Gecko's own profiles are 0700).
bool preparePersistentProfile(const QString &path, QString *error) {
  struct stat st {};
  if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
    if (errno == ENOENT && makePrivateDirectory(path)) return true;
    *error = QStringLiteral("cannot create the Gecko profile %1").arg(path);
    return false;
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0077) != 0) {
    *error = QStringLiteral("the Gecko profile %1 is not a private directory of this user").arg(path);
    return false;
  }
  return true;
}

// Another process holds Gecko's profile lock (a running Gecko). Only asks:
// nothing is locked or written.
bool profileInUse(const QString &profile) {
  const int fd = ::open(QFile::encodeName(profile + QStringLiteral("/.parentlock")).constData(),
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct flock lock {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  const bool held = ::fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK;
  ::close(fd);
  return held;
}

// Replaces <profile>/user.js atomically, unless a user.js DaliNira did not
// write is there.
bool writeUserJs(const QString &profile, const QByteArray &content, QString *error) {
  const QString path = profile + QStringLiteral("/user.js");
  const QByteArray marker(GeckoHostProcess::kUserJsMarker);
  struct stat st {};
  if (::lstat(QFile::encodeName(path).constData(), &st) == 0) {
    QFile existing(path);
    if (!S_ISREG(st.st_mode) || st.st_uid != ::getuid() || !existing.open(QIODevice::ReadOnly) ||
        existing.read(marker.size()) != marker) {
      *error = QStringLiteral("%1 was not written by DaliNira; it is left as it is").arg(path);
      return false;
    }
  }
  const QString temporary = path + QStringLiteral(".dalinira-new");
  ::unlink(QFile::encodeName(temporary).constData());  // a leftover of an interrupted launch
  const int fd = ::open(QFile::encodeName(temporary).constData(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  const QByteArray data = marker + content;
  const bool written = fd >= 0 && ::write(fd, data.constData(), data.size()) == data.size() && ::fsync(fd) == 0;
  if (fd >= 0) ::close(fd);
  if (!written || ::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) != 0) {
    ::unlink(QFile::encodeName(temporary).constData());
    *error = QStringLiteral("cannot write %1").arg(path);
    return false;
  }
  return true;
}

// Variables the host may inherit: locale, the session's display, D-Bus,
// audio, input method, fonts and proxies. Anything else (LD_*, GTK_MODULES,
// GIO/GST module paths, QT_*, MOZ_*) is dropped.
bool inheritable(const QString &name) {
  static const QSet<QString> names = {
      QStringLiteral("PATH"), QStringLiteral("USER"), QStringLiteral("LOGNAME"), QStringLiteral("LANG"),
      QStringLiteral("LANGUAGE"), QStringLiteral("TZ"), QStringLiteral("DISPLAY"), QStringLiteral("XAUTHORITY"),
      QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
      QStringLiteral("XDG_CURRENT_DESKTOP"), QStringLiteral("XDG_SESSION_DESKTOP"), QStringLiteral("DESKTOP_SESSION"),
      QStringLiteral("GTK_IM_MODULE"), QStringLiteral("XMODIFIERS"), QStringLiteral("GTK_THEME"),
      QStringLiteral("PULSE_SERVER"), QStringLiteral("FONTCONFIG_FILE"), QStringLiteral("FONTCONFIG_PATH"),
      QStringLiteral("http_proxy"), QStringLiteral("https_proxy"), QStringLiteral("ftp_proxy"),
      QStringLiteral("no_proxy"), QStringLiteral("all_proxy"), QStringLiteral("HTTP_PROXY"),
      QStringLiteral("HTTPS_PROXY"), QStringLiteral("FTP_PROXY"), QStringLiteral("NO_PROXY"),
      QStringLiteral("ALL_PROXY")};
  return names.contains(name) || name.startsWith(QLatin1String("LC_"));
}

}  // namespace

GeckoHostProcess::GeckoHostProcess(QObject *parent) : QObject(parent) {
  connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    qCDebug(lcGeckoHost) << "host" << pid_ << "exited, code" << code << "crashed" << (status == QProcess::CrashExit)
                         << "expected" << stopping_;
    emit exited(code, status);
  });
}

GeckoHostProcess::~GeckoHostProcess() {
  if (isRunning()) stop(0);
}

QProcessEnvironment GeckoHostProcess::environmentFor(const QProcessEnvironment &system, const Config &config,
                                                     const QString &homeDirectory, QString *error) {
  QProcessEnvironment env;
  for (const QString &name : system.keys()) {
    if (inheritable(name)) env.insert(name, system.value(name));
  }
  // The launch's own HOME and XDG directories: nothing Gecko keeps outside
  // its profile lands in the user's home.
  env.insert(QStringLiteral("HOME"), homeDirectory);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), homeDirectory + QStringLiteral("/.config"));
  env.insert(QStringLiteral("XDG_CACHE_HOME"), homeDirectory + QStringLiteral("/.cache"));
  env.insert(QStringLiteral("XDG_DATA_HOME"), homeDirectory + QStringLiteral("/.local/share"));
  env.insert(QStringLiteral("XDG_STATE_HOME"), homeDirectory + QStringLiteral("/.local/state"));
  env.insert(QStringLiteral("TMPDIR"), QFileInfo(homeDirectory).path() + QStringLiteral("/tmp"));
  env.insert(QStringLiteral("MOZ_CRASHREPORTER_DISABLE"), QStringLiteral("1"));
  // Native pending-update processing (the policy disables the update service).
  env.insert(QStringLiteral("MOZ_DISABLE_UPDATE_PROCESSING"), QStringLiteral("1"));
  // A crashing development build exits instead of waiting for a debugger.
  env.insert(QStringLiteral("MOZ_GDB_SLEEP"), QStringLiteral("0"));
  env.insert(QStringLiteral("DNG_START_URL"), QStringLiteral("about:blank"));
  env.insert(QStringLiteral("DNG_IPC_SOCKET"), config.socketPath);
  env.insert(QStringLiteral("DNG_IPC_TOKEN_FILE"), config.tokenFile);
  if (config.testInput) env.insert(QStringLiteral("DNG_TEST_INPUT"), QStringLiteral("1"));
  if (config.display == Display::Headless) {
    env.remove(QStringLiteral("DISPLAY"));
    env.remove(QStringLiteral("XAUTHORITY"));
    env.insert(QStringLiteral("MOZ_HEADLESS"), QStringLiteral("1"));
    // The graphics probe exits before touching any GPU, EGL or GLX.
    env.insert(QStringLiteral("MOZ_AVOID_OPENGL_ALTOGETHER"), QStringLiteral("1"));
  } else {
    if (env.value(QStringLiteral("DISPLAY")).isEmpty()) {
      if (error) *error = QStringLiteral("the Gecko host needs an X11 display (XWayland on Wayland sessions)");
      return {};
    }
    // The host's window lives on the X11 server DaliNira embeds it from.
    // WAYLAND_DISPLAY is not inherited: with it, both EGL vendors take their
    // Wayland path for the X11 display and crash or fall back to software.
    env.insert(QStringLiteral("GDK_BACKEND"), QStringLiteral("x11"));
    env.insert(QStringLiteral("EGL_PLATFORM"), QStringLiteral("x11"));
  }
  return env;
}

QStringList GeckoHostProcess::argumentsFor(const Config &config, const QString &profileDirectory) {
  // -app must be the first argument (nsBrowserApp.cpp). --new-instance with
  // a profile of its own never talks to another Gecko instance.
  return {QStringLiteral("-app"), config.runtime.applicationIni, QStringLiteral("--new-instance"),
          QStringLiteral("-profile"), profileDirectory};
}

bool GeckoHostProcess::start(const Config &config, QString *error) {
  if (isRunning()) {
    *error = QStringLiteral("the Gecko host is already running");
    return false;
  }
  if (!config.runtime.isValid()) {
    *error = QStringLiteral("the Gecko runtime was not validated");
    return false;
  }
  const QString run = config.runDirectory;
  const QString home = run + QStringLiteral("/home");
  const bool persistent = !config.profileDirectory.isEmpty();
  profileDirectory_ = persistent ? config.profileDirectory : run + QStringLiteral("/profile");
  if (persistent) {
    if (!preparePersistentProfile(profileDirectory_, error)) return false;
    if (profileInUse(profileDirectory_)) {
      *error = QStringLiteral("the Gecko profile %1 is in use by another Gecko").arg(profileDirectory_);
      return false;
    }
  }
  QStringList directories{home, home + QStringLiteral("/.config"), home + QStringLiteral("/.cache"),
                          home + QStringLiteral("/.local"), home + QStringLiteral("/.local/share"),
                          home + QStringLiteral("/.local/state"), run + QStringLiteral("/tmp")};
  if (!persistent) directories.prepend(profileDirectory_);
  for (const QString &dir : std::as_const(directories)) {
    if (!makePrivateDirectory(dir)) {
      *error = QStringLiteral("cannot create %1").arg(dir);
      return false;
    }
  }

  // Gecko's policy engine reads the host's update-disable policy from here
  // before the update service starts (profile-after-change).
  const QByteArray policyPath =
      QJsonDocument(QJsonArray{config.runtime.updatePolicyFile}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
  const QByteArray userJs = "user_pref(\"browser.policies.alternatePath\", " + policyPath + ");\n"
                            "user_pref(\"toolkit.policies.perUserDir\", false);\n";
  if (!writeUserJs(profileDirectory_, userJs, error)) return false;

  const QString log = config.logFile.isEmpty() ? run + QStringLiteral("/host-output.log") : config.logFile;
  const int logFd = ::open(QFile::encodeName(log).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (logFd < 0) {
    *error = QStringLiteral("cannot create the host log %1").arg(log);
    return false;
  }
  ::close(logFd);

  const QProcessEnvironment env = environmentFor(QProcessEnvironment::systemEnvironment(), config, home, error);
  if (env.isEmpty()) return false;
  process_.setProcessEnvironment(env);
  process_.setProgram(config.runtime.executable);
  process_.setArguments(argumentsFor(config, profileDirectory_));
  process_.setWorkingDirectory(run);
  process_.setProcessChannelMode(QProcess::MergedChannels);
  process_.setStandardOutputFile(log);
  process_.setStandardInputFile(QProcess::nullDevice());
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  process_.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors |
                                    QProcess::UnixProcessFlag::ResetSignalHandlers);
#endif
  // The host never outlives this process, even if it is SIGKILLed: the
  // parent-death signal, checked against a parent that already exited
  // before prctl ran. No core files (a Gecko core dump costs gigabytes).
  const pid_t parent = ::getpid();
  process_.setChildProcessModifier([parent] {
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent) ::_exit(127);
    const rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
  });
  stopping_ = false;
  process_.start();
  if (!process_.waitForStarted(10000)) {
    *error = process_.errorString();
    return false;
  }
  pid_ = process_.processId();
  qCDebug(lcGeckoHost) << "started host" << pid_ << "in" << run;
  return true;
}

QList<GeckoHostProcess::ProcessId> GeckoHostProcess::descendants() const {
  QList<ProcessId> result;
  if (pid_ <= 0) return result;
  QHash<qint64, ProcStat> table;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs)) {
    bool numeric = false;
    const qint64 pid = entry.toLongLong(&numeric);
    if (!numeric) continue;
    const ProcStat stat = readStat(pid);
    if (stat.valid) table.insert(pid, stat);
  }
  QList<qint64> frontier{pid_};
  QSet<qint64> seen{pid_};
  while (!frontier.isEmpty()) {
    const qint64 parent = frontier.takeFirst();
    for (auto it = table.cbegin(); it != table.cend(); ++it) {
      if (it.value().ppid == parent && !seen.contains(it.key())) {
        seen.insert(it.key());
        frontier.append(it.key());
        result.append({it.key(), it.value().startTime});
      }
    }
  }
  return result;
}

QJsonObject GeckoHostProcess::stop(int graceMs) {
  stopping_ = true;
  const QList<ProcessId> tree = descendants();
  QJsonArray treePids;
  for (const ProcessId &id : tree) treePids.append(id.pid);
  QString how = QStringLiteral("already-exited");
  if (isRunning()) {
    how = QStringLiteral("graceful");
    if (graceMs <= 0 || !process_.waitForFinished(graceMs)) {
      how = QStringLiteral("sigterm");
      ::kill(static_cast<pid_t>(pid_), SIGTERM);
      if (!process_.waitForFinished(5000)) {
        how = QStringLiteral("sigkill");
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
        process_.waitForFinished(5000);
      }
    }
  }
  // Content processes exit by themselves once the parent's channels close.
  QElapsedTimer timer;
  timer.start();
  QList<ProcessId> leftovers;
  do {
    leftovers.clear();
    for (const ProcessId &id : tree) {
      if (stillSameProcess(id)) leftovers.append(id);
    }
    if (leftovers.isEmpty()) break;
    QThread::msleep(100);
  } while (timer.elapsed() < 10000);
  QJsonArray killed;
  for (const ProcessId &id : std::as_const(leftovers)) {
    if (stillSameProcess(id)) {
      ::kill(static_cast<pid_t>(id.pid), SIGKILL);
      killed.append(id.pid);
    }
  }
  const QJsonObject result{{"hostPid", pid_}, {"how", how}, {"descendantsAtStop", treePids}, {"leftoversKilled", killed}};
  qCDebug(lcGeckoHost) << "stopped host" << result;
  return result;
}

}  // namespace dalinira::engine
