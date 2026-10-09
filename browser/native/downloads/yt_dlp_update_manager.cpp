#include "yt_dlp_update_manager.h"
#include "i18n/i18n.h"

#include "security_utils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSslError>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTimer>

#include <cstdio>
#include <memory>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

constexpr qint64 kMaximumReleaseJsonBytes = 2 * 1024 * 1024;
constexpr qint64 kMaximumChecksumsBytes = 128 * 1024;
constexpr qint64 kMinimumArtifactBytes = 4 * 1024;
constexpr qint64 kMaximumArtifactBytes = 64 * 1024 * 1024;
constexpr int kMaximumRedirects = 5;
constexpr qint64 kUpdateIntervalSeconds = 24 * 60 * 60;
const QUrl kLatestReleaseApi(QStringLiteral("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"));

QString executableSuffix() {
#if defined(Q_OS_WIN)
  return QStringLiteral(".exe");
#else
  return {};
#endif
}

QString normalizedDigest(QString digest) {
  digest = digest.trimmed().toLower();
  if (digest.startsWith(QStringLiteral("sha256:"))) digest.remove(0, 7);
  static const QRegularExpression valid(QStringLiteral("^[0-9a-f]{64}$"));
  return valid.match(digest).hasMatch() ? digest : QString{};
}

bool atomicallyReplace(const QString &source, const QString &destination) {
#if defined(Q_OS_WIN)
  return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  const QByteArray from = QFile::encodeName(source);
  const QByteArray to = QFile::encodeName(destination);
  return std::rename(from.constData(), to.constData()) == 0;
#endif
}

QProcessEnvironment verificationEnvironment(const QString &programPath) {
  const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
  QProcessEnvironment environment;
  for (const QString &name : {QStringLiteral("LANG"), QStringLiteral("LC_ALL"), QStringLiteral("LC_CTYPE"),
                              QStringLiteral("TMPDIR"), QStringLiteral("TEMP"), QStringLiteral("TMP"),
                              QStringLiteral("SSL_CERT_FILE"), QStringLiteral("SSL_CERT_DIR"),
                              QStringLiteral("SystemRoot"), QStringLiteral("WINDIR")}) {
    if (system.contains(name)) environment.insert(name, system.value(name));
  }
  QStringList paths{QFileInfo(programPath).absolutePath()};
#if defined(Q_OS_WIN)
  const QString systemRoot = system.value(QStringLiteral("SystemRoot"));
  if (!systemRoot.isEmpty()) paths << QDir(systemRoot).filePath(QStringLiteral("System32"));
#else
  paths << QStringLiteral("/usr/bin") << QStringLiteral("/bin");
#endif
  environment.insert(QStringLiteral("PATH"), paths.join(QDir::listSeparator()));
  return environment;
}

QJsonObject manifestEntry(const QString &file, const QString &version,
                          const QString &sha256, const QString &artifact) {
  return QJsonObject{{QStringLiteral("file"), file}, {QStringLiteral("version"), version},
                     {QStringLiteral("sha256"), sha256}, {QStringLiteral("artifact"), artifact}};
}

}  // namespace

YtDlpUpdateManager::YtDlpUpdateManager(const QString &managedDirectory, QObject *parent,
                                       QNetworkAccessManager *network)
    : QObject(parent),
      managedDirectory_(managedDirectory.isEmpty() ? defaultManagedDirectory()
                                                   : QDir::cleanPath(QFileInfo(managedDirectory).absoluteFilePath())),
      manifestPath_(QDir(managedDirectory_).filePath(QStringLiteral("yt-dlp-manifest.json"))),
      network_(network) {
  if (!network_) {
    network_ = new QNetworkAccessManager(this);
  }
  versionTimeout_ = new QTimer(this);
  versionTimeout_->setSingleShot(true);
  connect(versionTimeout_, &QTimer::timeout, this, [this] {
    if (versionProcess_) versionProcess_->kill();
  });
  networkTimeout_ = new QTimer(this);
  networkTimeout_->setSingleShot(true);
  connect(networkTimeout_, &QTimer::timeout, this, [this] {
    if (reply_) reply_->abort();
  });
}

YtDlpUpdateManager::~YtDlpUpdateManager() {
  cancel();
}

QString YtDlpUpdateManager::defaultManagedDirectory() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
      .filePath(QStringLiteral("tools"));
}

QString YtDlpUpdateManager::platformArtifactName(QString *error) {
  const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
#if defined(Q_OS_WIN)
  if (architecture == QLatin1String("x86_64") || architecture == QLatin1String("amd64"))
    return QStringLiteral("yt-dlp.exe");
  if (architecture == QLatin1String("arm64") || architecture == QLatin1String("aarch64"))
    return QStringLiteral("yt-dlp_arm64.exe");
#elif defined(Q_OS_LINUX)
  if (architecture == QLatin1String("x86_64") || architecture == QLatin1String("amd64"))
    return QStringLiteral("yt-dlp_linux");
  if (architecture == QLatin1String("arm64") || architecture == QLatin1String("aarch64"))
    return QStringLiteral("yt-dlp_linux_aarch64");
#elif defined(Q_OS_MACOS)
  if (architecture == QLatin1String("x86_64") || architecture == QLatin1String("arm64")
      || architecture == QLatin1String("aarch64")) return QStringLiteral("yt-dlp_macos");
#endif
  if (error) *error = I18n::text(QStringLiteral("media.platform_unsupported"));
  return {};
}

bool YtDlpUpdateManager::parseReleaseJson(const QByteArray &json, const QString &artifactName,
                                          YtDlpReleaseInfo *release, QString *error) {
  if (!release || artifactName.isEmpty()) return false;
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (!document.isObject()) {
    if (error) *error = I18n::text(QStringLiteral("media.release_info_unreadable"));
    return false;
  }
  const QJsonObject root = document.object();
  const QString tag = root.value(QStringLiteral("tag_name")).toString().trimmed();
  static const QRegularExpression versionPattern(QStringLiteral(R"(^\d{4}\.\d{2}\.\d{2}(?:\.\d+)?$)"));
  if (!versionPattern.match(tag).hasMatch() || root.value(QStringLiteral("draft")).toBool()
      || root.value(QStringLiteral("prerelease")).toBool()
      || (root.contains(QStringLiteral("immutable")) && !root.value(QStringLiteral("immutable")).toBool())) {
    if (error) *error = I18n::text(QStringLiteral("media.stable_release_unverified"));
    return false;
  }
  QJsonObject artifact;
  QJsonObject checksums;
  for (const QJsonValue &value : root.value(QStringLiteral("assets")).toArray()) {
    const QJsonObject asset = value.toObject();
    const QString name = asset.value(QStringLiteral("name")).toString();
    if (name == artifactName) artifact = asset;
    else if (name == QLatin1String("SHA2-256SUMS")) checksums = asset;
  }
  const QUrl artifactUrl(artifact.value(QStringLiteral("browser_download_url")).toString());
  const QUrl checksumsUrl(checksums.value(QStringLiteral("browser_download_url")).toString());
  const qint64 size = static_cast<qint64>(artifact.value(QStringLiteral("size")).toDouble());
  if (!artifactUrl.isValid() || !checksumsUrl.isValid() || size < kMinimumArtifactBytes
      || size > kMaximumArtifactBytes) {
    if (error) *error = I18n::text(QStringLiteral("media.artifact_not_found"));
    return false;
  }
  release->version = tag;
  release->artifactName = artifactName;
  release->artifactUrl = artifactUrl;
  release->artifactSize = size;
  release->apiSha256 = normalizedDigest(artifact.value(QStringLiteral("digest")).toString());
  release->checksumsUrl = checksumsUrl;
  if (error) error->clear();
  return true;
}

QString YtDlpUpdateManager::checksumForArtifact(const QByteArray &checksums, const QString &artifactName) {
  const QList<QByteArray> lines = checksums.split('\n');
  for (QByteArray line : lines) {
    line = line.trimmed();
    const int separator = line.indexOf(' ');
    if (separator != 64) continue;
    QByteArray name = line.mid(separator).trimmed();
    if (name.startsWith('*')) name.remove(0, 1);
    if (QString::fromUtf8(name) != artifactName) continue;
    return normalizedDigest(QString::fromLatin1(line.left(64)));
  }
  return {};
}

QString YtDlpUpdateManager::sha256File(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
  return QString::fromLatin1(hash.result().toHex());
}

bool YtDlpUpdateManager::verifyArtifactFile(const QString &path, const QString &artifactName,
                                            const QString &expectedSha256, QString *error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() < kMinimumArtifactBytes
      || file.size() > kMaximumArtifactBytes) {
    if (error) *error = I18n::text(QStringLiteral("media.downloaded_size_invalid"));
    return false;
  }
  const QByteArray magic = file.read(4);
  bool expectedType = false;
  if (artifactName.endsWith(QStringLiteral(".exe"))) expectedType = magic.startsWith("MZ");
  else if (artifactName.startsWith(QStringLiteral("yt-dlp_linux"))) expectedType = magic == QByteArray("\x7f" "ELF", 4);
  else if (artifactName == QLatin1String("yt-dlp_macos")) {
    static const QList<QByteArray> macMagics{QByteArray::fromHex("feedface"), QByteArray::fromHex("feedfacf"),
        QByteArray::fromHex("cefaedfe"), QByteArray::fromHex("cffaedfe"), QByteArray::fromHex("cafebabe"),
        QByteArray::fromHex("bebafeca")};
    expectedType = macMagics.contains(magic);
  }
  const QString expected = normalizedDigest(expectedSha256);
  if (!expectedType || expected.isEmpty() || sha256File(path) != expected) {
    if (error) *error = I18n::text(QStringLiteral("media.ytdlp_integrity_failed"));
    return false;
  }
  if (error) error->clear();
  return true;
}

QString YtDlpUpdateManager::managedDirectory() const { return managedDirectory_; }

QJsonObject YtDlpUpdateManager::readManifest() const {
  const QFileInfo manifestInfo(manifestPath_);
  if (manifestInfo.isSymLink()
      || (manifestInfo.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther))) return {};
  QFile file(manifestPath_);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) return {};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  const QJsonObject root = document.object();
  return root.value(QStringLiteral("version")).toInt() == 1 ? root : QJsonObject{};
}

QString YtDlpUpdateManager::verifiedManagedExecutable() const {
  const QJsonObject current = readManifest().value(QStringLiteral("current")).toObject();
  const QString fileName = current.value(QStringLiteral("file")).toString();
  const QString artifact = current.value(QStringLiteral("artifact")).toString();
  const QString expectedHash = current.value(QStringLiteral("sha256")).toString();
  static const QRegularExpression filePattern(QStringLiteral(R"(^yt-dlp-[0-9a-f]{16}(?:\.exe)?$)"));
  if (!filePattern.match(fileName).hasMatch() || artifact != platformArtifactName()) return {};
  const QString path = QDir(managedDirectory_).absoluteFilePath(fileName);
  const QFileInfo executableInfo(path);
  const QFileInfo directoryInfo(managedDirectory_);
  if (executableInfo.isSymLink()
      || executableInfo.absolutePath() != directoryInfo.absoluteFilePath()
      || executableInfo.canonicalPath() != directoryInfo.canonicalFilePath()) return {};
  QString error;
  if (!verifyArtifactFile(path, artifact, expectedHash, &error)) return {};
  return BrowserSecurity::resolveTrustedExecutable(fileName, {path}, false);
}

QString YtDlpUpdateManager::installedVersion() const {
  return verifiedManagedExecutable().isEmpty()
      ? QString{} : readManifest().value(QStringLiteral("current")).toObject().value(QStringLiteral("version")).toString();
}

bool YtDlpUpdateManager::updateCheckDue() const {
  const QDateTime checked = QDateTime::fromString(
      readManifest().value(QStringLiteral("checkedAt")).toString(), Qt::ISODate);
  return !checked.isValid() || checked.secsTo(QDateTime::currentDateTimeUtc()) >= kUpdateIntervalSeconds;
}

bool YtDlpUpdateManager::busy() const { return operation_ != Operation::None; }

bool YtDlpUpdateManager::prepareManagedDirectory(QString *error) const {
  QFileInfo info(managedDirectory_);
  if (info.exists() && (!info.isDir() || info.isSymLink())) {
    if (error) *error = I18n::text(QStringLiteral("media.tools_dir_insecure"));
    return false;
  }
  if (!info.exists() && !QDir().mkpath(managedDirectory_)) {
    if (error) *error = I18n::text(QStringLiteral("media.tools_dir_create_failed"));
    return false;
  }
  if (!QFile::setPermissions(managedDirectory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
    if (error) *error = I18n::text(QStringLiteral("media.tools_dir_permissions_failed"));
    return false;
  }
  return true;
}

bool YtDlpUpdateManager::isAllowedOfficialUrl(const QUrl &url) const {
  static const QSet<QString> hosts{QStringLiteral("api.github.com"), QStringLiteral("github.com"),
      QStringLiteral("objects.githubusercontent.com"), QStringLiteral("release-assets.githubusercontent.com")};
  return url.isValid() && url.scheme() == QLatin1String("https") && url.userInfo().isEmpty()
      && hosts.contains(url.host().toLower());
}

void YtDlpUpdateManager::ensureInstalled() {
  if (busy()) return;
  existingPath_ = verifiedManagedExecutable();
  if (!existingPath_.isEmpty()) {
    emit ready(existingPath_, false);
    if (updateCheckDue()) checkForUpdate();
    return;
  }
  begin(Operation::EnsureInstall);
}

void YtDlpUpdateManager::checkForUpdate() {
  if (busy()) return;
  existingPath_ = verifiedManagedExecutable();
  if (existingPath_.isEmpty()) { begin(Operation::EnsureInstall); return; }
  if (!updateCheckDue()) { emit updateCheckFinished(false); return; }
  begin(Operation::BackgroundUpdate);
}

void YtDlpUpdateManager::begin(Operation operation) {
  QString error;
  if (!prepareManagedDirectory(&error)) { emit failed(error, !existingPath_.isEmpty()); return; }
  if (platformArtifactName(&error).isEmpty()) { emit failed(error, !existingPath_.isEmpty()); return; }
  operation_ = operation;
  release_ = {};
  expectedSha256_.clear();
  emit statusChanged(operation == Operation::EnsureInstall
      ? I18n::text(QStringLiteral("media.engine_preparing")) : I18n::text(QStringLiteral("media.checking_ytdlp_update")), 0);
  request(RequestKind::LatestRelease, kLatestReleaseApi);
}

void YtDlpUpdateManager::request(RequestKind kind, const QUrl &url, int redirectCount) {
  if (!isAllowedOfficialUrl(url) || redirectCount > kMaximumRedirects) {
    fail(I18n::text(QStringLiteral("media.official_source_unverified")));
    return;
  }
  requestKind_ = kind;
  responseBytes_.clear();
  QNetworkRequest networkRequest(url);
  networkRequest.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DaliNira-Browser/6.1 yt-dlp-manager"));
  networkRequest.setRawHeader("Accept", "application/vnd.github+json");
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  reply_ = network_->get(networkRequest);
  networkTimeout_->start(120000);
  QNetworkReply *const launched = reply_;
  connect(reply_, &QNetworkReply::sslErrors, this, [launched](const QList<QSslError> &) { launched->abort(); });
  connect(reply_, &QNetworkReply::downloadProgress, this, [this, kind](qint64 received, qint64 total) {
    if (kind == RequestKind::Artifact && total > 0)
      emit statusChanged(I18n::text(QStringLiteral("media.downloading_ytdlp")), qBound(0, static_cast<int>(received * 100 / total), 99));
  });
  connect(reply_, &QNetworkReply::readyRead, this, [this, launched, kind] {
    if (reply_ != launched) return;
    const int status = launched->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) { launched->readAll(); return; }
    const QByteArray chunk = launched->readAll();
    if (kind == RequestKind::Artifact) {
      downloadedBytes_ += chunk.size();
      if (!temporaryFile_ || downloadedBytes_ > kMaximumArtifactBytes
          || temporaryFile_->write(chunk) != chunk.size()) launched->abort();
      else artifactHash_->addData(chunk);
    } else {
      responseBytes_ += chunk;
      const qint64 limit = kind == RequestKind::LatestRelease ? kMaximumReleaseJsonBytes : kMaximumChecksumsBytes;
      if (responseBytes_.size() > limit) launched->abort();
    }
  });
  connect(reply_, &QNetworkReply::finished, this, [this, launched, kind, redirectCount] {
    handleReplyFinished(launched, kind, redirectCount);
  });
}

void YtDlpUpdateManager::handleReplyFinished(QNetworkReply *finished, RequestKind kind, int redirectCount) {
  if (reply_ != finished) { finished->deleteLater(); return; }
  networkTimeout_->stop();
  reply_ = nullptr;
  const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QUrl redirect = finished->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
  const QNetworkReply::NetworkError networkError = finished->error();
  if (status >= 300 && status < 400 && redirect.isValid()) {
    const QUrl next = finished->url().resolved(redirect);
    finished->deleteLater();
    request(kind, next, redirectCount + 1);
    return;
  }
  finished->deleteLater();
  if (networkError != QNetworkReply::NoError || status != 200) {
    fail(I18n::text(QStringLiteral("media.github_unreachable")));
    return;
  }
  if (kind == RequestKind::LatestRelease) handleLatestRelease();
  else if (kind == RequestKind::Checksums) handleChecksums();
  else handleArtifactFinished();
}

void YtDlpUpdateManager::handleLatestRelease() {
  QString error;
  if (!parseReleaseJson(responseBytes_, platformArtifactName(), &release_, &error)
      || !isAllowedOfficialUrl(release_.artifactUrl) || !isAllowedOfficialUrl(release_.checksumsUrl)) {
    fail(error.isEmpty() ? I18n::text(QStringLiteral("media.release_info_unverified")) : error);
    return;
  }
  const QDate installed = versionDate(installedVersion());
  const QDate available = versionDate(release_.version);
  if (operation_ == Operation::BackgroundUpdate && installed.isValid() && available <= installed) {
    finishNoUpdate();
    return;
  }
  emit statusChanged(I18n::text(QStringLiteral("media.verifying_integrity")), 0);
  request(RequestKind::Checksums, release_.checksumsUrl);
}

void YtDlpUpdateManager::handleChecksums() {
  expectedSha256_ = checksumForArtifact(responseBytes_, release_.artifactName);
  if (expectedSha256_.isEmpty() || (!release_.apiSha256.isEmpty() && release_.apiSha256 != expectedSha256_)) {
    fail(I18n::text(QStringLiteral("media.checksum_unverified")));
    return;
  }
  beginArtifactDownload();
}

void YtDlpUpdateManager::beginArtifactDownload() {
  const QString pattern = QDir(managedDirectory_).filePath(
      QStringLiteral(".yt-dlp-download-XXXXXX") + executableSuffix());
  temporaryFile_ = new QTemporaryFile(pattern, this);
  temporaryFile_->setAutoRemove(true);
  if (!temporaryFile_->open()
      || !temporaryFile_->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    fail(I18n::text(QStringLiteral("media.temp_file_failed")));
    return;
  }
  artifactHash_ = new QCryptographicHash(QCryptographicHash::Sha256);
  downloadedBytes_ = 0;
  emit statusChanged(I18n::text(QStringLiteral("media.downloading_ytdlp")), 0);
  request(RequestKind::Artifact, release_.artifactUrl);
}

void YtDlpUpdateManager::handleArtifactFinished() {
  if (!temporaryFile_ || !artifactHash_ || downloadedBytes_ != release_.artifactSize
      || downloadedBytes_ < kMinimumArtifactBytes || downloadedBytes_ > kMaximumArtifactBytes) {
    fail(I18n::text(QStringLiteral("media.downloaded_size_unverified")));
    return;
  }
  temporaryFile_->flush();
  temporaryFile_->close();
  temporaryPath_ = temporaryFile_->fileName();
  temporaryFile_->setAutoRemove(false);
  delete temporaryFile_;
  temporaryFile_ = nullptr;
  const QString streamedHash = QString::fromLatin1(artifactHash_->result().toHex());
  QString error;
  if (streamedHash != expectedSha256_
      || !verifyArtifactFile(temporaryPath_, release_.artifactName, expectedSha256_, &error)) {
    fail(I18n::text(QStringLiteral("media.ytdlp_integrity_failed")));
    return;
  }
  if (!QFile::setPermissions(temporaryPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
    fail(I18n::text(QStringLiteral("media.exec_permission_failed")));
    return;
  }
  emit statusChanged(I18n::text(QStringLiteral("media.verifying_ytdlp")), 100);
  verifyDownloadedVersion();
}

void YtDlpUpdateManager::verifyDownloadedVersion() {
  versionOutput_.clear();
  versionProcess_ = new QProcess(this);
  QProcess *const launched = versionProcess_;
  versionProcess_->setProgram(temporaryPath_);
  versionProcess_->setArguments({QStringLiteral("--version")});
  versionProcess_->setProcessEnvironment(verificationEnvironment(temporaryPath_));
  connect(versionProcess_, &QProcess::readyReadStandardOutput, this, [this, launched] {
    if (versionProcess_ != launched) return;
    versionOutput_ += launched->readAllStandardOutput();
    if (versionOutput_.size() > 1024) launched->kill();
  });
  connect(versionProcess_, &QProcess::readyReadStandardError, this, [launched] { launched->readAllStandardError(); });
  connect(versionProcess_, &QProcess::errorOccurred, this, [this, launched](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart && versionProcess_ == launched) {
      fail(I18n::text(QStringLiteral("media.binary_unverified")));
    }
  });
  connect(versionProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, launched](int code, QProcess::ExitStatus status) {
    if (versionProcess_ != launched) return;
    versionTimeout_->stop();
    versionOutput_ += launched->readAllStandardOutput();
    launched->deleteLater();
    versionProcess_ = nullptr;
    const QString output = QString::fromUtf8(versionOutput_).trimmed();
    if (status != QProcess::NormalExit || code != 0 || output != release_.version) {
      fail(I18n::text(QStringLiteral("media.binary_version_unverified")));
      return;
    }
    installVerifiedArtifact(output);
  });
  versionProcess_->start();
  versionTimeout_->start(3000);
}

void YtDlpUpdateManager::installVerifiedArtifact(const QString &versionOutput) {
  const QString fileName = QStringLiteral("yt-dlp-") + expectedSha256_.left(16) + executableSuffix();
  const QString finalPath = QDir(managedDirectory_).filePath(fileName);
  bool movedTemporaryFile = false;
  if (QFileInfo::exists(finalPath)) {
    QString error;
    if (!verifyArtifactFile(finalPath, release_.artifactName, expectedSha256_, &error)) {
      fail(I18n::text(QStringLiteral("media.target_file_insecure")));
      return;
    }
  } else if (!atomicallyReplace(temporaryPath_, finalPath)) {
    fail(I18n::text(QStringLiteral("media.atomic_install_failed")));
    return;
  } else {
    movedTemporaryFile = true;
  }
  if (movedTemporaryFile) temporaryPath_.clear();
  const QJsonObject oldRoot = readManifest();
  const QJsonObject oldCurrent = oldRoot.value(QStringLiteral("current")).toObject();
  QJsonObject manifest{{QStringLiteral("version"), 1},
      {QStringLiteral("checkedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {QStringLiteral("current"), manifestEntry(fileName, versionOutput, expectedSha256_, release_.artifactName)}};
  if (!oldCurrent.isEmpty() && oldCurrent.value(QStringLiteral("file")).toString() != fileName)
    manifest.insert(QStringLiteral("previous"), oldCurrent);
  else if (!oldRoot.value(QStringLiteral("previous")).toObject().isEmpty())
    manifest.insert(QStringLiteral("previous"), oldRoot.value(QStringLiteral("previous")));
  if (!writeManifest(manifest)) {
    if (movedTemporaryFile) QFile::remove(finalPath);
    fail(I18n::text(QStringLiteral("media.install_record_failed")));
    return;
  }
  const QString verified = verifiedManagedExecutable();
  if (verified.isEmpty()) { fail(I18n::text(QStringLiteral("media.reverify_failed"))); return; }
  cleanupOldArtifacts(manifest);
  const bool updated = !existingPath_.isEmpty();
  cleanupTransfer();
  operation_ = Operation::None;
  emit statusChanged(I18n::text(QStringLiteral("media.ytdlp_ready")), 100);
  emit ready(verified, true);
  emit updateCheckFinished(updated);
}

bool YtDlpUpdateManager::writeManifest(const QJsonObject &manifest) const {
  QSaveFile file(manifestPath_);
  const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly)
      && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
      && file.write(bytes) == bytes.size() && file.commit();
}

void YtDlpUpdateManager::cleanupOldArtifacts(const QJsonObject &manifest) const {
  const QSet<QString> retained{manifest.value(QStringLiteral("current")).toObject().value(QStringLiteral("file")).toString(),
      manifest.value(QStringLiteral("previous")).toObject().value(QStringLiteral("file")).toString()};
  static const QRegularExpression artifactPattern(QStringLiteral(R"(^yt-dlp-[0-9a-f]{16}(?:\.exe)?$)"));
  const QDir directory(managedDirectory_);
  for (const QString &name : directory.entryList(QDir::Files | QDir::NoSymLinks)) {
    if (retained.contains(name) || !artifactPattern.match(name).hasMatch()) continue;
    QFile::remove(directory.absoluteFilePath(name));
  }
}

void YtDlpUpdateManager::finishNoUpdate() {
  QJsonObject manifest = readManifest();
  manifest.insert(QStringLiteral("checkedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  writeManifest(manifest);
  const QString path = existingPath_;
  cleanupTransfer();
  operation_ = Operation::None;
  emit statusChanged(I18n::text(QStringLiteral("media.ytdlp_up_to_date")), 100);
  emit ready(path, false);
  emit updateCheckFinished(false);
}

void YtDlpUpdateManager::fail(const QString &message) {
  const bool usable = !existingPath_.isEmpty() && !verifiedManagedExecutable().isEmpty();
  cleanupTransfer();
  const Operation operation = operation_;
  operation_ = Operation::None;
  emit failed(message, usable);
  if (operation == Operation::BackgroundUpdate) emit updateCheckFinished(false);
}

void YtDlpUpdateManager::cleanupTransfer() {
  if (reply_) {
    QNetworkReply *reply = reply_;
    reply_ = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
  }
  if (versionProcess_) {
    QProcess *process = versionProcess_;
    versionProcess_ = nullptr;
    process->disconnect(this);
    process->kill();
    process->deleteLater();
  }
  versionTimeout_->stop();
  networkTimeout_->stop();
  delete artifactHash_;
  artifactHash_ = nullptr;
  if (temporaryFile_) { delete temporaryFile_; temporaryFile_ = nullptr; }
  if (!temporaryPath_.isEmpty()) { QFile::remove(temporaryPath_); temporaryPath_.clear(); }
  responseBytes_.clear();
  versionOutput_.clear();
  downloadedBytes_ = 0;
}

void YtDlpUpdateManager::cancel() {
  if (!busy()) return;
  cleanupTransfer();
  operation_ = Operation::None;
}

QDate YtDlpUpdateManager::versionDate(const QString &version) {
  return QDate::fromString(version.left(10), QStringLiteral("yyyy.MM.dd"));
}
