#pragma once

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>

// The neutered replacement resources ("redirect resources") the blocker may
// substitute for a request: the files packaged in
// resources/adblock/web_accessible_resources, and nothing else. Generated
// rules name them by extensionPath; filter text ($redirect=, $rewrite=
// abp-resource:) names them by file name or a documented short name. Neither
// can supply a path or a URL, and the content always reaches the engines as a
// data: URL, which neither engine sends back through the filter.
//
// Thread-safe; each file is read once and its data: URL shared by every rule.
class DaliNiraBlockerRedirectResources final {
 public:
  explicit DaliNiraBlockerRedirectResources(const QString &directory);

  // The packaged file a filter token names, or empty.
  QString canonicalName(const QString &token) const;
  // data: URL of a packaged file, or empty (unknown, missing or too large).
  // WebKit loads it as the substitute; Chromium refuses to follow a
  // subresource redirect to data: and is served the same bytes instead by
  // DaliNiraBlockerResourceSchemeHandler.
  QString dataUrl(const QString &canonicalName) const;
  // The file itself and its MIME type; `ok` is false when unavailable.
  QByteArray content(const QString &canonicalName, QString *mimeType, bool *ok) const;
  QSet<QString> names() const { return files_; }

 private:
  struct Resource {
    bool loaded = false;
    QByteArray bytes;
    QString mimeType;
    QString dataUrl;
  };
  const Resource &load(const QString &canonicalName) const;  // mutex_ held

  QString directory_;
  QSet<QString> files_;
  mutable QMutex mutex_;
  mutable QHash<QString, Resource> resources_;
};
