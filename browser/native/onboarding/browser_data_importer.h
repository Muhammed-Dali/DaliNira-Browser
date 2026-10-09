#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>

namespace dalinira::onboarding {

struct ImportedHistoryEntry;

class IBrowserDataReceiver {
 public:
  virtual ~IBrowserDataReceiver() = default;
  virtual bool addBookmark(const QUrl &url, const QString &title, const QString &folder) = 0;
  virtual void createBookmarkFolder(const QString &folder) = 0;
  virtual void recordHistory(const QUrl &url, const QString &title, bool isReload = false) = 0;
  virtual void recordImportedHistory(const QUrl &url, const QString &title,
                                     const QDateTime &visitedAt, int visitCount,
                                     int typedCount) {
    Q_UNUSED(visitedAt);
    Q_UNUSED(visitCount);
    recordHistory(url, title, typedCount > 0);
  }
  virtual void importHistoryEntries(const QList<ImportedHistoryEntry> &entries);
};

struct DetectedBrowser {
  QString id;           // "chrome", "chromium", "brave", "firefox", "zen", "edge", "html_file"
  QString displayName;  // "Google Chrome", "Chromium", "Brave Browser", "Mozilla Firefox", "Zen Browser", etc.
  QString browserType;  // "chromium", "firefox", "html_file"
  QString profileName;
  QString iconName;     // Icon= value from the installed browser's .desktop file
  QString desktopFilePath;
  QStringList bookmarksPaths;
  QString bookmarksPath;
  QString historyPath;
  bool installed = false;
  bool hasBookmarks = false;
  bool hasHistory = false;
};

struct ImportedBookmark {
  QUrl url;
  QString title;
  QString folder;
};

struct ImportedHistoryEntry {
  QUrl url;
  QString title;
  QDateTime visitedAt;
  int visitCount = 1;
  int typedCount = 0;
};

class BrowserDataImporter {
 public:
  static QList<DetectedBrowser> detectBrowsers();

  // Pure parsing (independent of profile service for tests and previews)
  static QList<ImportedBookmark> parseBookmarksFromFile(const QString &filePath);
  static QList<ImportedBookmark> parseBookmarksFromJson(const QByteArray &jsonData);
  static QList<ImportedBookmark> parseBookmarksFromHtml(const QString &htmlContent);
  static QList<ImportedBookmark> parseBookmarksFromSqlite(const QString &sqlitePath);

  // Application into DaliNira's storage
  static int importBookmarks(const QString &filePath, IBrowserDataReceiver *receiver = nullptr);
  static int importBookmarks(const QStringList &filePaths, IBrowserDataReceiver *receiver = nullptr);
  static int importHistory(const QList<QPair<QUrl, QString>> &entries, IBrowserDataReceiver *receiver = nullptr);
  static QList<ImportedHistoryEntry> readHistoryFromDatabase(const QString &dbPath,
                                                             const QString &browserType,
                                                             int limit = 500);
  static int importHistoryFromDatabase(const QString &dbPath, const QString &browserType, IBrowserDataReceiver *receiver = nullptr);

};

}  // namespace dalinira::onboarding
