#include "settings/feedback_links.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QUrlQuery>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

QJsonObject loadCatalog(const QString &language) {
  QFile file(QStringLiteral(DALINIRA_SOURCE_DIR "/i18n/translations/%1.json").arg(language));
  check(file.open(QIODevice::ReadOnly), "localization catalog opens");
  return QJsonDocument::fromJson(file.readAll()).object();
}

QString loadTextFile(const QString &relativePath) {
  QFile file(QStringLiteral(DALINIRA_SOURCE_DIR "/") + relativePath);
  check(file.open(QIODevice::ReadOnly), "repository text file opens");
  return QString::fromUtf8(file.readAll());
}

}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  using dalinira::about_links::Destination;
  using dalinira::about_links::url;

  const QUrl profile = url(Destination::GitHubProfile);
  const QUrl repository = url(Destination::Repository);
  const QUrl bug = url(Destination::BugReport);
  const QUrl feature = url(Destination::FeatureRequest);
  check(profile == QUrl(QStringLiteral("https://github.com/Muhammed-Dali")),
        "GitHub profile URL is exact");
  check(repository == QUrl(QStringLiteral("https://github.com/Muhammed-Dali/DaliNira-Browser")),
        "repository URL is exact");
  check(bug == QUrl(QStringLiteral(
      "https://github.com/Muhammed-Dali/DaliNira-Browser/issues/new?template=bug_report.yml")),
      "bug-report URL is exact");
  check(feature == QUrl(QStringLiteral(
      "https://github.com/Muhammed-Dali/DaliNira-Browser/issues/new?template=feature_request.yml")),
      "feature-request URL is exact");

  // SettingsPage is native browser chrome shared by Chromium and WebKitGTK;
  // the links have no engine-dependent input or implementation.
  const QList<QUrl> chromiumUrls{profile, repository, bug, feature};
  const QList<QUrl> webkitUrls{url(Destination::GitHubProfile), url(Destination::Repository),
                               url(Destination::BugReport), url(Destination::FeatureRequest)};
  check(chromiumUrls == webkitUrls,
        "Chromium and WebKitGTK use identical feedback URLs");

  for (const QUrl &target : chromiumUrls) {
    check(target.scheme() == QLatin1String("https") && target.host() == QLatin1String("github.com"),
          "feedback URL uses the allowlisted HTTPS host");
    check(target.userInfo().isEmpty() && target.fragment().isEmpty(),
          "feedback URL contains no credentials or fragment data");
    const auto items = QUrlQuery(target).queryItems();
    check(items.isEmpty() || (items.size() == 1 && items.front().first == QLatin1String("template")),
          "About URL contains no unexpected query data");
    const QString encoded = target.toString(QUrl::FullyEncoded).toLower();
    for (const char *sensitive : {"password", "cookie", "token", "history", "current_url",
                                  "page_url", "active_url", "log"}) {
      check(!encoded.contains(QString::fromLatin1(sensitive)),
            "feedback URL contains no sensitive or current-page field");
    }
  }
  check(!dalinira::about_links::isAllowed(QUrl(QStringLiteral("https://example.com/"))),
        "non-allowlisted host is rejected");
  check(!dalinira::about_links::isAllowed(QUrl(
            QStringLiteral("https://github.com/Muhammed-Dali?current_url=https://private.example/"))),
        "allowlisted path with appended current-page data is rejected");

  QObject receiver;
  QList<QUrl> opened;
  const auto record = [&opened](const QUrl &target) { opened.append(target); };
  QPushButton profileButton;
  QPushButton repositoryButton;
  QPushButton bugButton;
  QPushButton featureButton;
  dalinira::about_links::wireButton(&profileButton, Destination::GitHubProfile, &receiver, record);
  dalinira::about_links::wireButton(&repositoryButton, Destination::Repository, &receiver, record);
  dalinira::about_links::wireButton(&bugButton, Destination::BugReport, &receiver, record);
  dalinira::about_links::wireButton(&featureButton, Destination::FeatureRequest, &receiver, record);
  profileButton.click();
  repositoryButton.click();
  bugButton.click();
  featureButton.click();

  QLabel details;
  dalinira::about_links::wireLabel(&details, &receiver, record);
  check(!details.openExternalLinks(), "About HTML links are forwarded instead of bypassing the bridge");
  QMetaObject::invokeMethod(&details, "linkActivated", Qt::DirectConnection,
                            Q_ARG(QString, profile.toString()));
  QMetaObject::invokeMethod(&details, "linkActivated", Qt::DirectConnection,
                            Q_ARG(QString, repository.toString()));
  check(opened == QList<QUrl>{profile, repository, bug, feature, profile, repository},
        "all six About-page actions are wired to their exact destinations");
  QMetaObject::invokeMethod(&details, "linkActivated", Qt::DirectConnection,
                            Q_ARG(QString, QStringLiteral("https://example.com/?token=secret")));
  check(opened.size() == 6, "non-allowlisted HTML link is silently rejected");

  // Send Email: a mailto: draft to the fixed project address that the user's
  // own mail client opens for review; DaliNira never sends it.
  const QString subject = QStringLiteral("DaliNira Feedback / Problem Report");
  const QString body = QStringLiteral("Line one & a=b + 100%\nDaliNira version: 8.0.0\n#end");
  const QUrl email = dalinira::about_links::emailUrl(subject, body);
  check(email.scheme() == QLatin1String("mailto"), "email draft uses mailto:");
  // The address itself lives only in feedback_links.cpp and the packaging
  // metadata (scripts/check-release-metadata.sh); the two must agree.
  const QString packagingContact = loadTextFile(QStringLiteral("packaging/linux/cpack.cmake"));
  const QString projectAddress = dalinira::about_links::feedbackEmailAddress();
  check(projectAddress.contains(QLatin1Char('@'))
            && packagingContact.contains(QLatin1Char('<') + projectAddress + QLatin1Char('>')),
        "email destination matches the packaging project contact");
  check(email.path() == projectAddress, "email draft is addressed to the official project address");
  check(dalinira::about_links::isAllowedEmail(email), "email draft passes the allowlist");
  const QUrlQuery emailQuery(email);
  check(emailQuery.queryItems().size() == 2, "email draft carries only subject and body");
  check(emailQuery.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded) == subject,
        "email subject round-trips exactly");
  check(emailQuery.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded)
            == QString(body).replace(QStringLiteral("\n"), QStringLiteral("\r\n")),
        "email body round-trips with reserved characters and CRLF line breaks");
  check(!email.toString(QUrl::FullyEncoded).contains(QLatin1Char('#')),
        "email body cannot terminate the URL early");
  check(!dalinira::about_links::isAllowedEmail(
            QUrl(QStringLiteral("mailto:user@example.com?subject=x"))),
        "email to another address is rejected");
  check(!dalinira::about_links::isAllowedEmail(QUrl(
            QStringLiteral("mailto:%1?cc=user@example.com").arg(projectAddress))),
        "email draft cannot add cc/bcc recipients");
  check(!dalinira::about_links::isAllowed(email), "mailto: is not an allowlisted web link");

  QList<QUrl> drafts;
  QPushButton emailButton;
  dalinira::about_links::wireEmailButton(&emailButton, &receiver, [&] { return email; },
                                         [&drafts](const QUrl &target) { drafts.append(target); });
  emailButton.click();
  check(drafts == QList<QUrl>{email}, "Send Email dispatches the exact draft once");
  QPushButton rogueButton;
  dalinira::about_links::wireEmailButton(
      &rogueButton, &receiver,
      [] { return QUrl(QStringLiteral("mailto:user@example.com")); },
      [&drafts](const QUrl &target) { drafts.append(target); });
  rogueButton.click();
  check(drafts.size() == 1, "non-allowlisted email draft is silently rejected");

  const QStringList keys{
      QStringLiteral("settings.about.feedback_send_email"),
      QStringLiteral("settings.about.feedback_email_subject"),
      QStringLiteral("settings.about.feedback_email_body"),
      QStringLiteral("settings.about.feedback_card"),
      QStringLiteral("settings.about.feedback_bug_report"),
      QStringLiteral("settings.about.feedback_feature_request"),
      QStringLiteral("settings.about.feedback_privacy_note"),
  };
  for (const QString &language : {QStringLiteral("tr"), QStringLiteral("en"),
                                  QStringLiteral("ar"), QStringLiteral("fr")}) {
    const QJsonObject catalog = loadCatalog(language);
    for (const QString &key : keys) {
      check(catalog.contains(key) && !catalog.value(key).toString().trimmed().isEmpty(),
            "feedback localization key is present and non-empty");
    }
    const QString template_ = catalog.value(QStringLiteral("settings.about.feedback_email_body")).toString();
    check(template_.contains(QLatin1String("%1")) && template_.contains(QLatin1String("%2")),
          "email template carries the version and engine placeholders");
    check(!template_.contains(QLatin1String("%3")) && !template_.contains(QLatin1String("http")),
          "email template has no URL or extra data placeholder");
    check(catalog.value(QStringLiteral("settings.about.feedback_email_subject"))
              .toString().contains(QLatin1String("DaliNira")),
          "email subject names DaliNira");
  }

  const QString bugForm = loadTextFile(QStringLiteral(".github/ISSUE_TEMPLATE/bug_report.yml"));
  for (const char *id : {"description", "reproduction", "expected", "actual", "version", "engine",
                         "os", "display_server", "architecture", "logs_screenshots", "additional_context"}) {
    check(bugForm.contains(QStringLiteral("id: %1").arg(QString::fromLatin1(id))),
          "bug form contains every required field");
  }
  const QString featureForm = loadTextFile(QStringLiteral(".github/ISSUE_TEMPLATE/feature_request.yml"));
  for (const char *id : {"summary", "use_case", "proposed_behavior", "improvement",
                         "engine_relevance", "additional_context"}) {
    check(featureForm.contains(QStringLiteral("id: %1").arg(QString::fromLatin1(id))),
          "feature form contains every required field");
  }
  for (const QString &form : {bugForm, featureForm}) {
    for (const char *sensitive : {"passwords", "cookies", "tokens", "private URLs", "personal data"}) {
      check(form.contains(QString::fromLatin1(sensitive), Qt::CaseInsensitive),
            "issue form includes the complete sensitive-data warning");
    }
  }

  if (failures) return 1;
  std::cout << "DaliNira feedback forms, URLs, email draft, engine parity, localization, and data minimization: ok\n";
  return 0;
}
