#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QCache>
#include <QStringList>
#include <functional>

class QNetworkReply;

// One stateless downloader per profile, shared by every search surface.
class SearchSuggestionService final : public QObject {
 public:
  using Completion = std::function<void(const QStringList &)>;
  explicit SearchSuggestionService(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
  void request(QObject *owner, const QString &query, const QString &engine,
               bool privateMode, Completion completion);
  void cancel();
  void setEnabled(bool enabled);
  bool isEnabled() const { return enabled_; }
  static QStringList parseResponse(const QByteArray &bytes);
  static QUrl endpoint(const QString &engine, const QString &query);
  static bool safeQuery(const QString &query);
  // True for input the omnibox would open as an address rather than search.
  static bool looksLikeUrl(const QString &query);
  static constexpr int MaxResponseBytes = 64 * 1024;
  static constexpr int MaxSuggestions = 8;
 private:
  void start();
  QNetworkAccessManager *network_;
  QTimer debounce_;
  QPointer<QNetworkReply> reply_;
  QPointer<QObject> owner_;
  Completion completion_;
  QString query_, engine_;
  QByteArray bytes_;
  quint64 generation_ = 0;
  bool enabled_ = false;
  QCache<QString, QStringList> cache_{32};
};
