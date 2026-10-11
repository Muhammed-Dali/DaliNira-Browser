#include "browser_data_importer.h"
#include "i18n/i18n.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimeZone>

#include <sqlite3.h>

#include <memory>
#include <algorithm>
#include <functional>

namespace dalinira::onboarding {

namespace {

struct SqliteCloser {
  void operator()(sqlite3 *db) const {
    if (db) sqlite3_close(db);
  }
};
using SqliteDb = std::unique_ptr<sqlite3, SqliteCloser>;

SqliteDb consistentDatabaseSnapshot(const QString &sourcePath) {
  sqlite3 *sourceRaw = nullptr;
  const QByteArray encodedPath = QFile::encodeName(sourcePath);
  if (sqlite3_open_v2(encodedPath.constData(), &sourceRaw,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
    if (sourceRaw) sqlite3_close(sourceRaw);
    return {};
  }
  SqliteDb source(sourceRaw);

  sqlite3 *snapshotRaw = nullptr;
  if (sqlite3_open_v2(":memory:", &snapshotRaw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    if (snapshotRaw) sqlite3_close(snapshotRaw);
    return {};
  }
  SqliteDb snapshot(snapshotRaw);
  sqlite3_backup *backup = sqlite3_backup_init(snapshot.get(), "main", source.get(), "main");
  if (!backup) return {};
  int stepResult = SQLITE_OK;
  for (int retry = 0; retry < 20; ++retry) {
    stepResult = sqlite3_backup_step(backup, -1);
    if (stepResult != SQLITE_BUSY && stepResult != SQLITE_LOCKED) break;
    sqlite3_sleep(5);
  }
  const int finishResult = sqlite3_backup_finish(backup);
  if ((stepResult != SQLITE_DONE && stepResult != SQLITE_OK) || finishResult != SQLITE_OK) {
    return {};
  }
  return snapshot;
}

QString sqliteText(sqlite3_stmt *statement, int column) {
  const auto *text = sqlite3_column_text(statement, column);
  return text ? QString::fromUtf8(reinterpret_cast<const char *>(text)) : QString();
}

struct SystemBrowserEntry {
  QString id;
  QString displayName;
  QString iconName;
  QString desktopFilePath;
};

QHash<QString, QString> readDesktopEntry(const QString &filePath) {
  QHash<QString, QString> values;
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return values;

  QTextStream stream(&file);
  bool inDesktopEntry = false;
  while (!stream.atEnd()) {
    const QString line = stream.readLine().trimmed();
    if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
      inDesktopEntry = line == QLatin1String("[Desktop Entry]");
      continue;
    }
    if (!inDesktopEntry || line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
    const qsizetype separator = line.indexOf(QLatin1Char('='));
    if (separator <= 0) continue;
    const QString key = line.left(separator).trimmed();
    if (!key.contains(QLatin1Char('['))) {
      values.insert(key, line.mid(separator + 1).trimmed());
    }
  }
  return values;
}

QString canonicalBrowserId(const QString &desktopFilePath,
                           const QString &displayName,
                           const QString &exec) {
  const QString signature = (QFileInfo(desktopFilePath).completeBaseName() + QLatin1Char(' ') +
                             displayName + QLatin1Char(' ') + exec).toLower();
  if (signature.contains(QLatin1String("brave"))) return QStringLiteral("brave");
  if (signature.contains(QLatin1String("google.chrome")) ||
      signature.contains(QLatin1String("google-chrome")) ||
      signature.contains(QLatin1String("google chrome"))) return QStringLiteral("chrome");
  if (signature.contains(QLatin1String("chromium"))) return QStringLiteral("chromium");
  if (signature.contains(QLatin1String("zen_browser")) ||
      signature.contains(QLatin1String("zen-browser")) ||
      signature.contains(QLatin1String("zen browser"))) return QStringLiteral("zen");
  if (signature.contains(QLatin1String("firefox"))) return QStringLiteral("firefox");
  if (signature.contains(QLatin1String("microsoft.edge")) ||
      signature.contains(QLatin1String("microsoft-edge")) ||
      signature.contains(QLatin1String("microsoft edge"))) return QStringLiteral("edge");
  if (signature.contains(QLatin1String("vivaldi"))) return QStringLiteral("vivaldi");
  if (signature.contains(QLatin1String("opera"))) return QStringLiteral("opera");
  if (signature.contains(QLatin1String("yandex"))) return QStringLiteral("yandex");
  if (signature.contains(QLatin1String("thorium"))) return QStringLiteral("thorium");
  if (signature.contains(QLatin1String("librewolf"))) return QStringLiteral("librewolf");
  if (signature.contains(QLatin1String("floorp"))) return QStringLiteral("floorp");
  if (signature.contains(QLatin1String("waterfox"))) return QStringLiteral("waterfox");
  if (signature.contains(QLatin1String("mullvad"))) return QStringLiteral("mullvad");
  if (signature.contains(QLatin1String("falkon"))) return QStringLiteral("falkon");

  QString id = QFileInfo(desktopFilePath).completeBaseName().toLower();
  id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
  return QStringLiteral("system_") + id;
}

QList<SystemBrowserEntry> installedSystemBrowsers() {
  QStringList applicationDirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
  applicationDirs << QStringLiteral("/usr/share/applications")
                  << QStringLiteral("/usr/local/share/applications")
                  << QStringLiteral("/var/lib/flatpak/exports/share/applications")
                  << QDir::homePath() + QStringLiteral("/.local/share/applications")
                  << QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications")
                  << QStringLiteral("/var/lib/snapd/desktop/applications");
  applicationDirs.removeDuplicates();

  QList<SystemBrowserEntry> entries;
  QSet<QString> seenDesktopFiles;
  QSet<QString> seenBrowserIds;
  for (const QString &applicationDir : applicationDirs) {
    const QDir dir(applicationDir);
    if (!dir.exists()) continue;
    const QFileInfoList desktopFiles = dir.entryInfoList(
        {QStringLiteral("*.desktop")}, QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &desktopFileInfo : desktopFiles) {
      const QString canonicalPath = desktopFileInfo.canonicalFilePath();
      const QString desktopFilePath = canonicalPath.isEmpty() ? desktopFileInfo.absoluteFilePath()
                                                               : canonicalPath;
      if (seenDesktopFiles.contains(desktopFilePath)) continue;
      seenDesktopFiles.insert(desktopFilePath);

      const auto desktopFile = readDesktopEntry(desktopFilePath);
      const bool hidden = desktopFile.value(QStringLiteral("Hidden")).compare(
                              QStringLiteral("true"), Qt::CaseInsensitive) == 0;
      const QString type = desktopFile.value(QStringLiteral("Type"));
      const QString categories = desktopFile.value(QStringLiteral("Categories"));
      const QString mimeTypes = desktopFile.value(QStringLiteral("MimeType"));
      const QString displayName = desktopFile.value(QStringLiteral("Name")).trimmed();
      const QString exec = desktopFile.value(QStringLiteral("Exec"));
      const QString iconName = desktopFile.value(QStringLiteral("Icon")).trimmed();

      const bool isWebBrowser = categories.split(QLatin1Char(';'), Qt::SkipEmptyParts)
                                    .contains(QStringLiteral("WebBrowser"), Qt::CaseInsensitive);
      const bool handlesWebLinks = mimeTypes.contains(QLatin1String("x-scheme-handler/http"));
      if (hidden || type != QLatin1String("Application") || displayName.isEmpty() ||
          (!isWebBrowser && !handlesWebLinks)) {
        continue;
      }

      const QString id = canonicalBrowserId(desktopFilePath, displayName, exec);
      if (id.contains(QLatin1String("dalinira")) || seenBrowserIds.contains(id)) continue;
      seenBrowserIds.insert(id);
      entries.append({id, displayName, iconName, desktopFilePath});
    }
  }
  return entries;
}

const SystemBrowserEntry *systemBrowser(const QList<SystemBrowserEntry> &entries, const QString &id) {
  for (const auto &entry : entries) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

void applySystemMetadata(DetectedBrowser &browser, const SystemBrowserEntry *entry,
                         const QString &fallbackIconName) {
  browser.installed = entry != nullptr;
  browser.iconName = entry && !entry->iconName.isEmpty() ? entry->iconName : fallbackIconName;
  if (entry) {
    browser.displayName = entry->displayName;
    browser.desktopFilePath = entry->desktopFilePath;
  }
}

QString stableProfileId(const QString &browserId, const QString &profilePath, bool firstProfile) {
  if (firstProfile) return browserId;
  const QByteArray path = QFile::encodeName(QDir::cleanPath(profilePath));
  return browserId + QLatin1Char('_') +
         QString::fromLatin1(QCryptographicHash::hash(path, QCryptographicHash::Sha256).toHex().left(12));
}

QHash<QString, QString> chromiumProfileNames(const QString &userDataRoot) {
  QHash<QString, QString> names;
  QFile localState(QDir(userDataRoot).filePath(QStringLiteral("Local State")));
  if (!localState.open(QIODevice::ReadOnly)) return names;
  const QJsonObject infoCache = QJsonDocument::fromJson(localState.readAll()).object()
                                    .value(QStringLiteral("profile")).toObject()
                                    .value(QStringLiteral("info_cache")).toObject();
  for (auto it = infoCache.begin(); it != infoCache.end(); ++it) {
    const QString name = it.value().toObject().value(QStringLiteral("name")).toString().trimmed();
    names.insert(it.key(), name.isEmpty() ? it.key() : name);
  }
  return names;
}

void appendChromiumProfiles(QList<DetectedBrowser> &detected,
                            const QList<SystemBrowserEntry> &systemBrowsers,
                            const QString &browserId, const QString &fallbackName,
                            const QString &fallbackIcon, const QStringList &userDataRoots) {
  struct ProfileFiles {
    QString path;
    QString name;
    QStringList bookmarks;
    QString history;
  };
  QList<ProfileFiles> profiles;
  QSet<QString> seenPaths;
  for (const QString &rootPath : userDataRoots) {
    const QDir root(rootPath);
    if (!root.exists()) continue;
    const auto names = chromiumProfileNames(rootPath);
    QStringList candidates = names.keys();
    candidates << QStringLiteral("Default");
    const QFileInfoList directories = root.entryInfoList(
        {QStringLiteral("Profile *")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &directory : directories) candidates << directory.fileName();
    if (QFile::exists(root.filePath(QStringLiteral("History"))) ||
        QFile::exists(root.filePath(QStringLiteral("Bookmarks"))) ||
        QFile::exists(root.filePath(QStringLiteral("AccountBookmarks")))) {
      candidates.prepend(QStringLiteral("."));
    }
    candidates.removeDuplicates();

    for (const QString &candidate : candidates) {
      const QString profilePath = candidate == QLatin1String(".")
          ? root.absolutePath() : root.filePath(candidate);
      const QString canonicalPath = QDir::cleanPath(profilePath);
      if (seenPaths.contains(canonicalPath) || !QDir(profilePath).exists()) continue;
      QStringList bookmarkFiles;
      for (const QString &fileName : {QStringLiteral("Bookmarks"), QStringLiteral("AccountBookmarks")}) {
        const QString path = QDir(profilePath).filePath(fileName);
        if (QFile::exists(path)) bookmarkFiles.append(path);
      }
      const QString historyPath = QDir(profilePath).filePath(QStringLiteral("History"));
      if (bookmarkFiles.isEmpty() && !QFile::exists(historyPath)) continue;
      seenPaths.insert(canonicalPath);
      const QString profileName = candidate == QLatin1String(".")
          ? QStringLiteral("Default") : names.value(candidate, candidate);
      profiles.append({canonicalPath, profileName, bookmarkFiles,
                       QFile::exists(historyPath) ? historyPath : QString()});
    }
  }

  std::sort(profiles.begin(), profiles.end(), [](const ProfileFiles &left,
                                                  const ProfileFiles &right) {
    return left.path < right.path;
  });

  const auto *systemEntry = systemBrowser(systemBrowsers, browserId);
  for (int index = 0; index < profiles.size(); ++index) {
    const auto &profile = profiles.at(index);
    DetectedBrowser browser;
    browser.id = stableProfileId(browserId, profile.path, index == 0);
    browser.displayName = fallbackName;
    browser.browserType = QStringLiteral("chromium");
    browser.profileName = profile.name;
    browser.bookmarksPaths = profile.bookmarks;
    browser.bookmarksPath = profile.bookmarks.value(0);
    browser.historyPath = profile.history;
    browser.hasBookmarks = !profile.bookmarks.isEmpty();
    browser.hasHistory = !profile.history.isEmpty();
    applySystemMetadata(browser, systemEntry, fallbackIcon);
    if (profiles.size() > 1) {
      browser.displayName += QStringLiteral(" — ") + profile.name;
    }
    detected.append(browser);
  }
}

void appendFirefoxProfiles(QList<DetectedBrowser> &detected,
                           const QList<SystemBrowserEntry> &systemBrowsers,
                           const QString &browserId, const QString &fallbackName,
                           const QString &fallbackIcon, const QStringList &profileRoots) {
  struct ProfileFiles { QString path; QString name; QString places; };
  QList<ProfileFiles> profiles;
  QSet<QString> seenPaths;
  for (const QString &rootPath : profileRoots) {
    const QDir root(rootPath);
    if (!root.exists()) continue;
    const QFileInfoList directories = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &directory : directories) {
      const QString profilePath = QDir::cleanPath(directory.absoluteFilePath());
      const QString placesPath = QDir(profilePath).filePath(QStringLiteral("places.sqlite"));
      if (!QFile::exists(placesPath) || seenPaths.contains(profilePath)) continue;
      seenPaths.insert(profilePath);
      QString profileName = directory.fileName();
      const qsizetype separator = profileName.indexOf(QLatin1Char('.'));
      if (separator >= 0 && separator + 1 < profileName.size()) profileName = profileName.mid(separator + 1);
      profiles.append({profilePath, profileName, placesPath});
    }
  }

  std::sort(profiles.begin(), profiles.end(), [](const ProfileFiles &left,
                                                  const ProfileFiles &right) {
    return left.path < right.path;
  });

  const auto *systemEntry = systemBrowser(systemBrowsers, browserId);
  for (int index = 0; index < profiles.size(); ++index) {
    const auto &profile = profiles.at(index);
    DetectedBrowser browser;
    browser.id = stableProfileId(browserId, profile.path, index == 0);
    browser.displayName = fallbackName;
    browser.browserType = browserId == QLatin1String("zen")
        ? QStringLiteral("zen") : QStringLiteral("firefox");
    browser.profileName = profile.name;
    browser.bookmarksPaths = {profile.places};
    browser.bookmarksPath = profile.places;
    browser.historyPath = profile.places;
    browser.hasBookmarks = true;
    browser.hasHistory = true;
    applySystemMetadata(browser, systemEntry, fallbackIcon);
    if (profiles.size() > 1) browser.displayName += QStringLiteral(" — ") + profile.name;
    detected.append(browser);
  }
}

void appendFalkonProfiles(QList<DetectedBrowser> &detected,
                          const QList<SystemBrowserEntry> &systemBrowsers,
                          const QStringList &profileRoots) {
  struct ProfileFiles { QString path; QString name; QString bookmarks; };
  QList<ProfileFiles> profiles;
  QSet<QString> seenPaths;
  for (const QString &rootPath : profileRoots) {
    const QDir root(rootPath);
    if (!root.exists()) continue;
    for (const QFileInfo &directory : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      const QString profilePath = QDir::cleanPath(directory.absoluteFilePath());
      const QString bookmarksPath = QDir(profilePath).filePath(QStringLiteral("bookmarks.json"));
      if (!QFile::exists(bookmarksPath) || seenPaths.contains(profilePath)) continue;
      seenPaths.insert(profilePath);
      profiles.append({profilePath, directory.fileName(), bookmarksPath});
    }
  }
  std::sort(profiles.begin(), profiles.end(), [](const ProfileFiles &left,
                                                  const ProfileFiles &right) {
    return left.path < right.path;
  });

  const auto *systemEntry = systemBrowser(systemBrowsers, QStringLiteral("falkon"));
  for (int index = 0; index < profiles.size(); ++index) {
    const auto &profile = profiles.at(index);
    DetectedBrowser browser;
    browser.id = stableProfileId(QStringLiteral("falkon"), profile.path, index == 0);
    browser.displayName = QStringLiteral("Falkon");
    browser.browserType = QStringLiteral("falkon");
    browser.profileName = profile.name;
    browser.bookmarksPaths = {profile.bookmarks};
    browser.bookmarksPath = profile.bookmarks;
    browser.hasBookmarks = true;
    applySystemMetadata(browser, systemEntry, QStringLiteral("falkon"));
    if (profiles.size() > 1) browser.displayName += QStringLiteral(" — ") + profile.name;
    detected.append(browser);
  }
}

}  // namespace

void IBrowserDataReceiver::importHistoryEntries(const QList<ImportedHistoryEntry> &entries) {
  for (const auto &entry : entries) {
    recordImportedHistory(entry.url, entry.title, entry.visitedAt,
                          entry.visitCount, entry.typedCount);
  }
}

QList<DetectedBrowser> BrowserDataImporter::detectBrowsers() {
  QList<DetectedBrowser> detected;
  const QString home = QDir::homePath();
  const QList<SystemBrowserEntry> systemBrowsers = installedSystemBrowsers();

  // Chromium-family browsers use the same Bookmarks JSON and History SQLite
  // schemas. Enumerate every real profile, including Flatpak/Snap layouts.
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("brave"),
      QStringLiteral("Brave Browser"), QStringLiteral("brave-desktop"), {
        home + QStringLiteral("/.config/BraveSoftware/Brave-Browser"),
        home + QStringLiteral("/.config/BraveSoftware/Brave-Browser-Beta"),
        home + QStringLiteral("/.var/app/com.brave.Browser/config/BraveSoftware/Brave-Browser"),
        home + QStringLiteral("/snap/brave/current/.config/BraveSoftware/Brave-Browser")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("chrome"),
      QStringLiteral("Google Chrome"), QStringLiteral("com.google.Chrome"), {
        home + QStringLiteral("/.config/google-chrome"),
        home + QStringLiteral("/.config/google-chrome-beta"),
        home + QStringLiteral("/.config/google-chrome-unstable"),
        home + QStringLiteral("/.var/app/com.google.Chrome/config/google-chrome"),
        home + QStringLiteral("/.var/app/com.google.ChromeBeta/config/google-chrome-beta"),
        home + QStringLiteral("/.var/app/com.google.ChromeDev/config/google-chrome-unstable"),
        home + QStringLiteral("/snap/google-chrome/current/.config/google-chrome")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("chromium"),
      QStringLiteral("Chromium"), QStringLiteral("chromium"), {
        home + QStringLiteral("/.config/chromium"),
        home + QStringLiteral("/.var/app/org.chromium.Chromium/config/chromium"),
        home + QStringLiteral("/.var/app/io.github.ungoogled_software.ungoogled_chromium/config/chromium"),
        home + QStringLiteral("/snap/chromium/current/.config/chromium")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("edge"),
      QStringLiteral("Microsoft Edge"), QStringLiteral("microsoft-edge"), {
        home + QStringLiteral("/.config/microsoft-edge"),
        home + QStringLiteral("/.config/microsoft-edge-beta"),
        home + QStringLiteral("/.var/app/com.microsoft.Edge/config/microsoft-edge")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("vivaldi"),
      QStringLiteral("Vivaldi"), QStringLiteral("vivaldi"), {
        home + QStringLiteral("/.config/vivaldi"),
        home + QStringLiteral("/.config/vivaldi-snapshot"),
        home + QStringLiteral("/.var/app/com.vivaldi.Vivaldi/config/vivaldi")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("opera"),
      QStringLiteral("Opera"), QStringLiteral("opera"), {
        home + QStringLiteral("/.config/opera"),
        home + QStringLiteral("/.config/opera-beta"),
        home + QStringLiteral("/.var/app/com.opera.Opera/config/opera")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("yandex"),
      QStringLiteral("Yandex Browser"), QStringLiteral("yandex-browser"), {
        home + QStringLiteral("/.config/yandex-browser"),
        home + QStringLiteral("/.config/yandex-browser-beta"),
        home + QStringLiteral("/.var/app/ru.yandex.Browser/config/yandex-browser")});
  appendChromiumProfiles(detected, systemBrowsers, QStringLiteral("thorium"),
      QStringLiteral("Thorium"), QStringLiteral("thorium-browser"), {
        home + QStringLiteral("/.config/thorium"),
        home + QStringLiteral("/.config/Thorium"),
        home + QStringLiteral("/.var/app/org.thorium.Thorium/config/thorium")});

  // Firefox-family browsers share places.sqlite for bookmarks and history.
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("firefox"),
      QStringLiteral("Mozilla Firefox"), QStringLiteral("firefox"), {
        home + QStringLiteral("/.mozilla/firefox"),
        home + QStringLiteral("/.var/app/org.mozilla.firefox/.mozilla/firefox"),
        home + QStringLiteral("/snap/firefox/common/.mozilla/firefox")});
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("zen"),
      QStringLiteral("Zen Browser"), QStringLiteral("app.zen_browser.zen"), {
        home + QStringLiteral("/.zen"),
        home + QStringLiteral("/.var/app/app.zen_browser.zen/.zen")});
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("librewolf"),
      QStringLiteral("LibreWolf"), QStringLiteral("librewolf"), {
        home + QStringLiteral("/.librewolf"),
        home + QStringLiteral("/.var/app/io.gitlab.librewolf-community/.librewolf")});
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("floorp"),
      QStringLiteral("Floorp"), QStringLiteral("floorp"), {
        home + QStringLiteral("/.floorp"),
        home + QStringLiteral("/.var/app/one.ablaze.floorp/.floorp")});
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("waterfox"),
      QStringLiteral("Waterfox"), QStringLiteral("waterfox"), {
        home + QStringLiteral("/.waterfox"),
        home + QStringLiteral("/.var/app/net.waterfox.waterfox/.waterfox")});
  appendFirefoxProfiles(detected, systemBrowsers, QStringLiteral("mullvad"),
      QStringLiteral("Mullvad Browser"), QStringLiteral("mullvad-browser"), {
        home + QStringLiteral("/.mullvad/mullvad-browser"),
        home + QStringLiteral("/.var/app/net.mullvad.MullvadBrowser/.mullvad/mullvad-browser")});
  appendFalkonProfiles(detected, systemBrowsers, {
      home + QStringLiteral("/.config/falkon/profiles"),
      home + QStringLiteral("/.var/app/org.kde.falkon/config/falkon/profiles"),
      home + QStringLiteral("/snap/falkon/current/.config/falkon/profiles")});

  // Generic HTML bookmark file option (always available).
  DetectedBrowser htmlFile;
  htmlFile.id = QStringLiteral("html_file");
  htmlFile.displayName = dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_html_file"));
  htmlFile.browserType = QStringLiteral("html_file");
  htmlFile.iconName = QStringLiteral("text-html");
  htmlFile.hasBookmarks = true;
  htmlFile.hasHistory = false;
  detected.append(htmlFile);

  return detected;
}

QList<ImportedBookmark> BrowserDataImporter::parseBookmarksFromFile(const QString &filePath) {
  if (filePath.isEmpty() || !QFile::exists(filePath)) {
    return {};
  }
  if (filePath.endsWith(QStringLiteral(".sqlite")) || filePath.endsWith(QStringLiteral(".db"))) {
    return parseBookmarksFromSqlite(filePath);
  }
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray data = file.readAll();
  const QString trimmed = QString::fromUtf8(data).trimmed();

  if (trimmed.startsWith(QLatin1Char('{')) || trimmed.contains(QLatin1String("\"roots\""))) {
    return parseBookmarksFromJson(data);
  }
  return parseBookmarksFromHtml(trimmed);
}

QList<ImportedBookmark> BrowserDataImporter::parseBookmarksFromJson(const QByteArray &jsonData) {
  QList<ImportedBookmark> bookmarks;
  const QJsonDocument doc = QJsonDocument::fromJson(jsonData);
  if (!doc.isObject()) {
    return bookmarks;
  }
  const QJsonObject rootObj = doc.object();
  const QJsonObject roots = rootObj.value(QStringLiteral("roots")).toObject();

  std::function<void(const QJsonArray &, const QString &)> parseChildren;
  parseChildren = [&](const QJsonArray &children, const QString &folder) {
    for (const auto &val : children) {
      if (!val.isObject()) continue;
      const QJsonObject obj = val.toObject();
      const QString type = obj.value(QStringLiteral("type")).toString();
      const QString name = obj.value(QStringLiteral("name")).toString();

      if (type == QLatin1String("url")) {
        const QUrl url(obj.value(QStringLiteral("url")).toString());
        if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
          bookmarks.append({url, name, folder});
        }
      } else if (type == QLatin1String("folder")) {
        const QString subFolder = folder.isEmpty() ? name : (folder + QStringLiteral("/") + name);
        parseChildren(obj.value(QStringLiteral("children")).toArray(), subFolder);
      }
    }
  };

  for (const QString &rootKey : {QStringLiteral("bookmark_bar"), QStringLiteral("bookmark_menu"),
                                 QStringLiteral("other"), QStringLiteral("synced")}) {
    if (roots.contains(rootKey)) {
      const QJsonObject rootChild = roots.value(rootKey).toObject();
      QString rootName = rootChild.value(QStringLiteral("name")).toString().trimmed();
      if (rootName.isEmpty()) {
        if (rootKey == QLatin1String("bookmark_bar")) rootName = QStringLiteral("Bookmarks Bar");
        else if (rootKey == QLatin1String("bookmark_menu")) rootName = QStringLiteral("Bookmarks Menu");
        else if (rootKey == QLatin1String("other")) rootName = QStringLiteral("Other Bookmarks");
        else rootName = QStringLiteral("Synced Bookmarks");
      }
      parseChildren(rootChild.value(QStringLiteral("children")).toArray(), rootName);
    }
  }

  return bookmarks;
}

QList<ImportedBookmark> BrowserDataImporter::parseBookmarksFromHtml(const QString &htmlContent) {
  QList<ImportedBookmark> bookmarks;
  QStringList folderStack;
  QString pendingFolder;
  const QStringList lines = htmlContent.split(QLatin1Char('\n'));
  static const QRegularExpression folderRegex(QStringLiteral("<H3[^>]*>([^<]+)</H3>"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression bookmarkRegex(QStringLiteral("<A\\s+[^>]*HREF=[\"']([^\"']+)[\"'][^>]*>([^<]*)</A>"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression openDlRegex(QStringLiteral("<DL(?:\\s[^>]*)?>"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression closeDlRegex(QStringLiteral("</DL>"), QRegularExpression::CaseInsensitiveOption);

  for (const QString &line : lines) {
    const QString trimmed = line.trimmed();
    auto folderMatch = folderRegex.match(trimmed);
    if (folderMatch.hasMatch()) {
      pendingFolder = folderMatch.captured(1).trimmed();
    }
    if (openDlRegex.match(trimmed).hasMatch() && !pendingFolder.isEmpty()) {
      folderStack.append(pendingFolder);
      pendingFolder.clear();
    }
    auto bmMatch = bookmarkRegex.match(trimmed);
    if (bmMatch.hasMatch()) {
      const QUrl url(bmMatch.captured(1).trimmed());
      QString title = bmMatch.captured(2).trimmed();
      if (title.isEmpty()) title = url.host();
      if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
        bookmarks.append({url, title, folderStack.join(QLatin1Char('/'))});
      }
    }
    if (closeDlRegex.match(trimmed).hasMatch() && !folderStack.isEmpty()) {
      folderStack.removeLast();
    }
  }
  return bookmarks;
}

QList<ImportedBookmark> BrowserDataImporter::parseBookmarksFromSqlite(const QString &sqlitePath) {
  if (sqlitePath.isEmpty() || !QFile::exists(sqlitePath)) {
    return {};
  }
  SqliteDb database = consistentDatabaseSnapshot(sqlitePath);
  if (!database) return {};
  QList<ImportedBookmark> bookmarks;
  static constexpr auto sql =
      "WITH RECURSIVE folders(id, path) AS ("
      " SELECT id, COALESCE(NULLIF(title, ''), 'Bookmarks') FROM moz_bookmarks WHERE parent = 1 AND type = 2"
      " UNION ALL"
      " SELECT child.id, CASE WHEN parent.path = '' THEN COALESCE(child.title, '')"
      " ELSE parent.path || '/' || COALESCE(child.title, '') END"
      " FROM moz_bookmarks child JOIN folders parent ON child.parent = parent.id WHERE child.type = 2"
      ")"
      " SELECT p.url, COALESCE(NULLIF(b.title, ''), p.title, p.url), COALESCE(f.path, '')"
      " FROM moz_bookmarks b JOIN moz_places p ON b.fk = p.id"
      " LEFT JOIN folders f ON b.parent = f.id WHERE b.type = 1 LIMIT 10000";
  sqlite3_stmt *rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) return {};
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const QUrl url(sqliteText(statement.get(), 0));
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https"))) continue;
    QString title = sqliteText(statement.get(), 1).trimmed();
    if (title.isEmpty()) title = url.host();
    bookmarks.append({url, title, sqliteText(statement.get(), 2).trimmed()});
  }
  return bookmarks;
}

QList<ImportedHistoryEntry> BrowserDataImporter::readHistoryFromDatabase(
    const QString &dbPath, const QString &browserType, int limit) {
  if (dbPath.isEmpty() || !QFile::exists(dbPath) || limit <= 0) return {};
  SqliteDb database = consistentDatabaseSnapshot(dbPath);
  if (!database) return {};

  const bool isFirefox = browserType == QLatin1String("firefox") ||
                         browserType == QLatin1String("zen");
  const char *sql = isFirefox
      ? "SELECT url, COALESCE(title, url), last_visit_date, visit_count, typed"
        " FROM moz_places WHERE visit_count > 0 AND last_visit_date IS NOT NULL"
        " ORDER BY last_visit_date DESC LIMIT ?1"
      : "SELECT url, COALESCE(title, url), last_visit_time, visit_count, typed_count"
        " FROM urls WHERE visit_count > 0 AND last_visit_time > 0"
        " ORDER BY last_visit_time DESC LIMIT ?1";
  sqlite3_stmt *rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) return {};
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  sqlite3_bind_int(statement.get(), 1, limit);

  QList<ImportedHistoryEntry> entries;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const QUrl url(sqliteText(statement.get(), 0));
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https"))) continue;
    const qint64 rawTimestamp = sqlite3_column_int64(statement.get(), 2);
    const qint64 unixMilliseconds = isFirefox
        ? rawTimestamp / 1000
        : rawTimestamp / 1000 - 11644473600000LL;
    entries.append({url, sqliteText(statement.get(), 1),
                    QDateTime::fromMSecsSinceEpoch(unixMilliseconds, QTimeZone::utc()),
                    std::max(1, sqlite3_column_int(statement.get(), 3)),
                    std::max(0, sqlite3_column_int(statement.get(), 4))});
  }
  return entries;
}

int BrowserDataImporter::importHistoryFromDatabase(const QString &dbPath,
                                                   const QString &browserType,
                                                   IBrowserDataReceiver *receiver) {
  if (!receiver) return 0;
  const auto entries = readHistoryFromDatabase(dbPath, browserType);
  receiver->importHistoryEntries(entries);
  return entries.size();
}

int BrowserDataImporter::importBookmarks(const QString &filePath, IBrowserDataReceiver *receiver) {
  return importBookmarks(QStringList{filePath}, receiver);
}

int BrowserDataImporter::importBookmarks(const QStringList &filePaths,
                                         IBrowserDataReceiver *receiver) {
  if (!receiver || filePaths.isEmpty()) return 0;
  int count = 0;
  QSet<QString> seenUrls;
  QSet<QString> createdFolders;
  for (const QString &filePath : filePaths) {
    if (filePath.isEmpty()) continue;
    const auto bookmarks = parseBookmarksFromFile(filePath);
    for (const auto &bm : bookmarks) {
      const QString key = bm.url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
      if (key.isEmpty() || seenUrls.contains(key)) continue;
      seenUrls.insert(key);
      if (!bm.folder.isEmpty() && !createdFolders.contains(bm.folder)) {
        receiver->createBookmarkFolder(bm.folder);
        createdFolders.insert(bm.folder);
      }
      if (receiver->addBookmark(bm.url, bm.title, bm.folder)) ++count;
    }
  }
  return count;
}

int BrowserDataImporter::importHistory(const QList<QPair<QUrl, QString>> &entries, IBrowserDataReceiver *receiver) {
  if (!receiver) {
    return 0;
  }
  int count = 0;
  for (const auto &entry : entries) {
    if (entry.first.isValid()) {
      receiver->recordHistory(entry.first, entry.second, false);
      count++;
    }
  }
  return count;
}

}  // namespace dalinira::onboarding
