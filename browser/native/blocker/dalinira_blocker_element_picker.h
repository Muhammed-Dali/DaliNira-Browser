#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <functional>

class DaliNiraBlockerService;

// One "Block element" session on one page, shared by both engines.
//
// The picker itself runs in the page's isolated world (element_picker_runtime.h):
// hovering, selecting, candidate selectors and the preview never cross into
// native code. The session injects it, then polls it a few times a second
// for the user's decisions only (create / undo / closed). A created filter is
// "host##selector" for the page's host, validated here again, and saved as
// an ordinary custom filter through DaliNiraBlockerSettings, so the shared
// blocker recompiles and both engines apply it like any other cosmetic rule.
//
// The engine adapter is just `runner`: Chromium runs the script in
// QWebEngineScript::ApplicationWorld, WebKit in DaliNira's content world.
class DaliNiraElementPickerSession final : public QObject {
  Q_OBJECT
 public:
  using ScriptRunner = std::function<void(const QString &script, std::function<void(const QVariant &)> callback)>;

  // Why the picker cannot run on a page (empty: it can). The blocker must be
  // on for the site, since a rule it could not apply would look broken.
  static QString unavailableReason(const DaliNiraBlockerService *service, const QUrl &pageUrl);
  // The custom filter for `selector` on `pageUrl`, or empty with `error` set.
  // Only plain CSS selectors are accepted: no rule blocks, no procedural or
  // scriptlet syntax, bounded length.
  static QString filterFor(const QUrl &pageUrl, const QString &selector, QString *error = nullptr);
  static constexpr int kMaxSelectorLength = 256;

  DaliNiraElementPickerSession(DaliNiraBlockerService *service, const QUrl &pageUrl, ScriptRunner runner,
                               QObject *parent = nullptr);
  ~DaliNiraElementPickerSession() override;

  void start();
  // Leaves the page as it was (unless a filter was already created).
  void cancel();
  bool isActive() const { return active_; }
  QUrl pageUrl() const { return pageUrl_; }
  QString savedFilter() const { return savedFilter_; }

 signals:
  void filterSaved(const QString &filter);
  void filterRemoved(const QString &filter);
  void finished();

 private:
  void poll();
  void handleEvent(const QString &json);
  void finish();

  QPointer<DaliNiraBlockerService> service_;
  QUrl pageUrl_;
  ScriptRunner runner_;
  QTimer pollTimer_;
  bool active_ = false;
  bool pollPending_ = false;
  QString savedFilter_;
  bool savedFilterWasNew_ = false;
};
