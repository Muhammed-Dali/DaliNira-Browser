#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class CredentialVaultManager;
class QWidget;

namespace dalinira::engine { class WebKitEngineView; }

// WebKitGTK credential integration. The renderer bridge is registered only in
// DaliNira's isolated content world and this controller independently checks
// the actual top-level URI before accepting or releasing any secret.
class WebKitCredentialController final : public QObject {
  Q_OBJECT
 public:
  explicit WebKitCredentialController(CredentialVaultManager *vault, QWidget *dialogParent,
                                      QObject *parent = nullptr);
  void registerView(dalinira::engine::WebKitEngineView *view);
  // The view moved to another window, whose controller registers it.
  void unregisterView(dalinira::engine::WebKitEngineView *view);

 private:
  struct Candidate {
    QString origin;
    QString username;
    QString password;
    QDateTime expiresAt;
    quint64 generation = 0;
  };
  void handleMessage(dalinira::engine::WebKitEngineView *view, const QString &type,
                     const QVariantMap &payload);
  void refreshFillButton(dalinira::engine::WebKitEngineView *view);
  void clearView(dalinira::engine::WebKitEngineView *view);
  void promptSave(dalinira::engine::WebKitEngineView *view);
  void fill(dalinira::engine::WebKitEngineView *view, const QString &token);

  CredentialVaultManager *vault_ = nullptr;
  QWidget *dialogParent_ = nullptr;
  QHash<dalinira::engine::WebKitEngineView *, Candidate> candidates_;
  QHash<dalinira::engine::WebKitEngineView *, QString> fillTokens_;
  QHash<dalinira::engine::WebKitEngineView *, quint64> generations_;
};
