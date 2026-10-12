#include "adult_content_protection.h"
#include "core/application_identity.h"

#include "compact_domain_table.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTextStream>

namespace dalinira::core {

AdultContentProtectionService &AdultContentProtectionService::instance() {
  static AdultContentProtectionService s_instance;
  return s_instance;
}

AdultContentProtectionService::AdultContentProtectionService()
    : compactDomains_(std::make_unique<CompactDomainTable>()) {
  reload();
}

AdultContentProtectionService::~AdultContentProtectionService() = default;

QString AdultContentProtectionService::normalizeHost(const QString &rawHost) {
  QString h = rawHost.trimmed().toLower();
  if (h.isEmpty()) return {};

  // Strip port if present
  // Handle IPv6 literal [::1]:8080 vs standard domain:port
  if (h.startsWith(QLatin1Char('['))) {
    const int closeBracket = h.indexOf(QLatin1Char(']'));
    if (closeBracket > 0) {
      const int colon = h.indexOf(QLatin1Char(':'), closeBracket);
      if (colon > 0) {
        h = h.left(colon);
      }
    }
  } else {
    const int colon = h.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
      h = h.left(colon);
    }
  }

  // Strip trailing dots
  while (h.endsWith(QLatin1Char('.'))) {
    h.chop(1);
  }

  // Strip leading dots if any
  while (h.startsWith(QLatin1Char('.'))) {
    h.remove(0, 1);
  }

  return h;
}

bool AdultContentProtectionService::isValidCanonicalDomain(const QString &domain) {
  if (domain.isEmpty() || domain.length() > 253) return false;

  // Reject characters forbidden in canonical hostnames / URLs
  if (domain.contains(QLatin1Char(':')) ||
      domain.contains(QLatin1Char('/')) ||
      domain.contains(QLatin1Char('\\')) ||
      domain.contains(QLatin1Char('?')) ||
      domain.contains(QLatin1Char('#')) ||
      domain.contains(QLatin1Char('@')) ||
      domain.contains(QLatin1Char('%')) ||
      domain.contains(QLatin1Char('*')) ||
      domain.contains(QLatin1Char(' ')) ||
      domain.contains(QLatin1Char('\t')) ||
      domain.contains(QLatin1Char('\r')) ||
      domain.contains(QLatin1Char('\n'))) {
    return false;
  }

  // Reject IPv4 and IPv6 addresses
  QHostAddress addr(domain);
  if (!addr.isNull()) {
    return false;
  }

  // Reject localhost and loopback names
  if (domain == QLatin1String("localhost") || domain.endsWith(QLatin1String(".localhost"))) {
    return false;
  }

  // Reject internal / local / non-routable hostnames
  if (domain.endsWith(QLatin1String(".local")) ||
      domain.endsWith(QLatin1String(".internal")) ||
      domain.endsWith(QLatin1String(".lan")) ||
      domain.endsWith(QLatin1String(".home")) ||
      domain.endsWith(QLatin1String(".corp")) ||
      domain.endsWith(QLatin1String(".arpa"))) {
    return false;
  }

  // Must contain at least one dot separating domain and TLD
  const int firstDot = domain.indexOf(QLatin1Char('.'));
  if (firstDot <= 0 || firstDot == domain.length() - 1) {
    return false;
  }

  // Validate each label (RFC 1035 / RFC 1123)
  const QStringList labels = domain.split(QLatin1Char('.'), Qt::KeepEmptyParts);
  if (labels.size() < 2) return false;

  for (const QString &label : labels) {
    if (label.isEmpty() || label.length() > 63) return false;
    if (label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-'))) {
      return false;
    }
    for (const QChar &ch : label) {
      const char c = ch.toLatin1();
      const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      const bool isHyphen = (c == '-');
      if (!isAlphaNum && !isHyphen) {
        return false;
      }
    }
  }

  // TLD cannot be purely numeric
  const QString tld = labels.last();
  bool isAllDigits = true;
  for (const QChar &ch : tld) {
    if (!ch.isDigit()) {
      isAllDigits = false;
      break;
    }
  }
  if (isAllDigits) return false;

  return true;
}

bool AdultContentProtectionService::matchesDomain(const QString &normalizedHost, const QString &ruleDomain) {
  if (normalizedHost.isEmpty() || ruleDomain.isEmpty()) return false;
  if (normalizedHost == ruleDomain) return true;
  return normalizedHost.endsWith(QLatin1Char('.') + ruleDomain);
}

bool AdultContentProtectionService::isAllowedHost(const QString &rawHost) const {
  const QString h = normalizeHost(rawHost);
  if (h.isEmpty()) return false;

  QReadLocker locker(&lock_);
  if (allowlistDomains_.isEmpty()) return false;

  QString candidate = h;
  while (!candidate.isEmpty()) {
    if (allowlistDomains_.contains(candidate)) {
      return true;
    }
    const int dot = candidate.indexOf(QLatin1Char('.'));
    if (dot < 0) {
      break;
    }
    candidate = candidate.mid(dot + 1);
  }
  return false;
}

bool AdultContentProtectionService::isBlockedHost(const QString &rawHost) const {
  const QString h = normalizeHost(rawHost);
  if (h.isEmpty()) return false;

  // 1. Allowlist precedence: if matched in allowlist, do NOT block
  if (isAllowedHost(h)) {
    return false;
  }

  // 2. Blocklist matching
  QReadLocker locker(&lock_);
  if (compactDomains_ && compactDomains_->matchesDomain(h)) {
    return true;
  }
  if (canonicalDomains_.isEmpty()) return false;

  // Fast O(number_of_dots) suffix lookup in hash set
  QString candidate = h;
  while (!candidate.isEmpty()) {
    if (canonicalDomains_.contains(candidate)) {
      return true;
    }
    const int dot = candidate.indexOf(QLatin1Char('.'));
    if (dot < 0) {
      break;
    }
    candidate = candidate.mid(dot + 1);
  }
  return false;
}

bool AdultContentProtectionService::isBlocked(const QUrl &url) const {
  if (!url.isValid()) return false;

  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
    return false;
  }

  return isBlockedHost(url.host());
}

DomainListStats AdultContentProtectionService::loadFromFileWithStats(const QString &filePath) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[AdultProtection] Failed to open blocklist file:" << filePath;
    return {};
  }

  QStringList lines;
  QTextStream in(&file);
  while (!in.atEnd()) {
    lines.append(in.readLine());
  }

  return loadFromLinesWithStats(lines);
}

DomainListStats AdultContentProtectionService::loadFromLinesWithStats(const QStringList &lines) {
  DomainListStats stats;
  QSet<QString> loaded;
  loaded.reserve(lines.size());

  for (QString line : lines) {
    const int hashPos = line.indexOf(QLatin1Char('#'));
    if (hashPos >= 0) {
      line = line.left(hashPos);
    }
    const QString canonical = normalizeHost(line);
    if (canonical.isEmpty()) {
      continue;
    }
    if (!isValidCanonicalDomain(canonical)) {
      stats.invalidCount++;
      continue;
    }
    if (loaded.contains(canonical)) {
      stats.duplicateCount++;
    } else {
      loaded.insert(canonical);
      stats.validCount++;
    }
  }

  QWriteLocker locker(&lock_);
  compactDomains_->clear();
  canonicalDomains_ = std::move(loaded);
  return stats;
}

bool AdultContentProtectionService::loadSnapshot(const QString &filePath, qint64 *loadTimeMs) {
  auto loaded = std::make_unique<CompactDomainTable>();
  if (!loaded->loadSnapshot(filePath, loadTimeMs)) {
    qWarning() << "[AdultProtection] Rejected missing or invalid domain snapshot:" << filePath;
    QWriteLocker locker(&lock_);
    compactDomains_->clear();
    canonicalDomains_.clear();
    return false;
  }

  const size_t count = loaded->domainCount();
  QWriteLocker locker(&lock_);
  compactDomains_.swap(loaded);
  canonicalDomains_.clear();
  qInfo() << "[AdultProtection] Loaded validated domain snapshot:" << filePath
          << "domains:" << count
          << "load-ms:" << (loadTimeMs ? *loadTimeMs : -1);
  return true;
}

bool AdultContentProtectionService::loadFromFile(const QString &filePath) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[AdultProtection] Failed to open blocklist file:" << filePath;
    return false;
  }

  QStringList lines;
  QTextStream in(&file);
  while (!in.atEnd()) {
    lines.append(in.readLine());
  }
  loadFromLines(lines);
  return true;
}

void AdultContentProtectionService::loadFromLines(const QStringList &lines) {
  loadFromLinesWithStats(lines);
}

void AdultContentProtectionService::addDomain(const QString &domain) {
  const QString canonical = normalizeHost(domain);
  if (!isValidCanonicalDomain(canonical)) return;

  QWriteLocker locker(&lock_);
  canonicalDomains_.insert(canonical);
}

void AdultContentProtectionService::clear() {
  QWriteLocker locker(&lock_);
  compactDomains_->clear();
  canonicalDomains_.clear();
}

int AdultContentProtectionService::domainCount() const {
  QReadLocker locker(&lock_);
  return canonicalDomains_.size() + static_cast<int>(compactDomains_->domainCount());
}

void AdultContentProtectionService::setCustomFilePath(const QString &path) {
  QWriteLocker locker(&lock_);
  customFilePath_ = path;
}

// -------------------------------------------------------------
// Allowlist Management
// -------------------------------------------------------------

bool AdultContentProtectionService::loadAllowlistFromFile(const QString &filePath) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[AdultProtection] Failed to open allowlist file:" << filePath;
    return false;
  }

  QStringList lines;
  QTextStream in(&file);
  while (!in.atEnd()) {
    lines.append(in.readLine());
  }
  loadAllowlistFromLines(lines);
  return true;
}

void AdultContentProtectionService::loadAllowlistFromLines(const QStringList &lines) {
  QSet<QString> loaded;
  for (QString line : lines) {
    const int hashPos = line.indexOf(QLatin1Char('#'));
    if (hashPos >= 0) {
      line = line.left(hashPos);
    }
    const QString canonical = normalizeHost(line);
    if (!canonical.isEmpty() && isValidCanonicalDomain(canonical)) {
      loaded.insert(canonical);
    }
  }

  QWriteLocker locker(&lock_);
  allowlistDomains_ = std::move(loaded);
}

void AdultContentProtectionService::addAllowDomain(const QString &domain) {
  const QString canonical = normalizeHost(domain);
  if (!isValidCanonicalDomain(canonical)) return;

  QWriteLocker locker(&lock_);
  allowlistDomains_.insert(canonical);
}

void AdultContentProtectionService::clearAllowlist() {
  QWriteLocker locker(&lock_);
  allowlistDomains_.clear();
}

int AdultContentProtectionService::allowlistDomainCount() const {
  QReadLocker locker(&lock_);
  return allowlistDomains_.size();
}

void AdultContentProtectionService::setCustomAllowlistFilePath(const QString &path) {
  QWriteLocker locker(&lock_);
  customAllowlistFilePath_ = path;
}

QString AdultContentProtectionService::resolveSnapshotPath() const {
  if (!customFilePath_.isEmpty() && QFile::exists(customFilePath_)) {
    return customFilePath_;
  }

  const QString appDir = dalinira::application_identity::applicationDirectory();
  const QStringList candidates = {
      appDir + QStringLiteral("/resources/privacy/adult_domains.bin"),
      QDir::currentPath() + QStringLiteral("/build/resources/privacy/adult_domains.bin"),
      QStringLiteral(":/privacy/adult_domains.bin")
  };

  for (const QString &path : candidates) {
    if (QFile::exists(path)) {
      return path;
    }
  }
  return {};
}

QString AdultContentProtectionService::resolveAllowlistFilePath() const {
  if (!customAllowlistFilePath_.isEmpty() && QFile::exists(customAllowlistFilePath_)) {
    return customAllowlistFilePath_;
  }

  const QString appDir = dalinira::application_identity::applicationDirectory();
  const QStringList candidates = {
      appDir + QStringLiteral("/resources/privacy/adult_allowlist.txt"),
      appDir + QStringLiteral("/../browser/resources/privacy/adult_allowlist.txt"),
      appDir + QStringLiteral("/../../browser/resources/privacy/adult_allowlist.txt"),
      QDir::currentPath() + QStringLiteral("/browser/resources/privacy/adult_allowlist.txt"),
      QDir::currentPath() + QStringLiteral("/build/resources/privacy/adult_allowlist.txt"),
      QStringLiteral(":/privacy/adult_allowlist.txt")
  };

  for (const QString &path : candidates) {
    if (QFile::exists(path)) {
      return path;
    }
  }
  return {};
}

void AdultContentProtectionService::reloadAllowlist() {
  const QString path = resolveAllowlistFilePath();
  if (!path.isEmpty()) {
    loadAllowlistFromFile(path);
  }
}

void AdultContentProtectionService::reload() {
  const QString path = resolveSnapshotPath();
  if (path.isEmpty()) {
    qWarning() << "[AdultProtection] Domain snapshot not found; protection is fail-open.";
    clear();
  } else {
    qint64 loadTimeMs = 0;
    loadSnapshot(path, &loadTimeMs);
  }
  reloadAllowlist();
}

}  // namespace dalinira::core
