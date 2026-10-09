#pragma once

#include <QWidget>

#include <functional>

class CredentialVaultManager;
class QLineEdit;
class QListWidget;
class QLabel;
class QIcon;
class QUrl;
class QWebEngineProfile;

class PasswordManagerPage final : public QWidget {
  Q_OBJECT
 public:
  using FaviconResultCallback = std::function<void(const QIcon &, const QUrl &)>;
  using FaviconLookup = std::function<void(const QUrl &, FaviconResultCallback)>;

  explicit PasswordManagerPage(CredentialVaultManager *vault,
                               QWebEngineProfile *webProfile = nullptr,
                               QWidget *parent = nullptr);
  void refresh();
  void setFaviconLookupForTesting(FaviconLookup lookup);

 private:
  void showSetup();
  void showConsent();
  void showPrivateBrowsingNotice();
  void showUnlock();
  void showRecords();
  void createVault();
  void exportBackup();
  void importBackup();
  void addCredential(const QString &id = {});
  void copyPassword(const QString &id);
  bool ensureSensitiveAuthorization(const QString &vaultId);
  void requestCachedFavicon(QLabel *label, const QString &origin);
  CredentialVaultManager *vault_ = nullptr;
  QWebEngineProfile *webProfile_ = nullptr;
  FaviconLookup faviconLookup_;
  QLineEdit *master_ = nullptr;
  QLineEdit *confirm_ = nullptr;
  QLineEdit *search_ = nullptr;
  QListWidget *records_ = nullptr;
  QLabel *status_ = nullptr;
  QString statusMessage_;
  bool usernamesVisible_ = true;
};
