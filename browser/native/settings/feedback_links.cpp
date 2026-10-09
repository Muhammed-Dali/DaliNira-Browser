#include "feedback_links.h"

#include <QAbstractButton>
#include <QLabel>
#include <QObject>
#include <QUrlQuery>

#include <utility>

namespace dalinira::about_links {

namespace {

constexpr auto kFeedbackEmail = "dali.nira.12.1453kerinyoli@gmail.com";
constexpr auto kIssueFormBase = "https://github.com/Muhammed-Dali/DaliNira-Browser/issues/new";

QString templateName(Destination destination) {
  switch (destination) {
    case Destination::BugReport:
      return QStringLiteral("bug_report.yml");
    case Destination::FeatureRequest:
      return QStringLiteral("feature_request.yml");
    case Destination::GitHubProfile:
    case Destination::Repository:
      return {};
  }
  return {};
}

void dispatch(const QUrl &candidate, const UrlHandler &handler) {
  if (handler && isAllowed(candidate)) handler(candidate);
}

}  // namespace

QUrl url(Destination destination) {
  if (destination == Destination::GitHubProfile)
    return QUrl(QStringLiteral("https://github.com/Muhammed-Dali"));
  if (destination == Destination::Repository)
    return QUrl(QStringLiteral("https://github.com/Muhammed-Dali/DaliNira-Browser"));

  const QString issueTemplate = templateName(destination);
  QUrl url(QString::fromLatin1(kIssueFormBase));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("template"), issueTemplate);
  url.setQuery(query);
  return url;
}

bool isAllowed(const QUrl &candidate) {
  if (!candidate.isValid() || candidate.scheme() != QLatin1String("https")
      || candidate.host() != QLatin1String("github.com") || !candidate.userInfo().isEmpty()
      || !candidate.fragment().isEmpty()) {
    return false;
  }
  for (const Destination destination : {Destination::GitHubProfile, Destination::Repository,
                                        Destination::BugReport, Destination::FeatureRequest}) {
    if (candidate == url(destination)) return true;
  }
  return false;
}

void wireButton(QAbstractButton *button, Destination destination, QObject *context,
                UrlHandler handler) {
  if (!button || !context) return;
  QObject::connect(button, &QAbstractButton::clicked, context,
                   [destination, handler = std::move(handler)] { dispatch(url(destination), handler); });
}

QString feedbackEmailAddress() { return QString::fromLatin1(kFeedbackEmail); }

QUrl emailUrl(const QString &subject, const QString &body) {
  // RFC 6068: every reserved character is percent-encoded and line breaks are
  // CRLF; the localized template itself uses plain newlines.
  QString crlfBody = body;
  crlfBody.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  crlfBody.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
  QUrl url;
  url.setScheme(QStringLiteral("mailto"));
  url.setPath(feedbackEmailAddress());
  url.setQuery(QString::fromLatin1("subject=" + QUrl::toPercentEncoding(subject)
                                   + "&body=" + QUrl::toPercentEncoding(crlfBody)));
  return url;
}

bool isAllowedEmail(const QUrl &candidate) {
  if (!candidate.isValid() || candidate.scheme() != QLatin1String("mailto")
      || candidate.path() != feedbackEmailAddress() || !candidate.fragment().isEmpty()) {
    return false;
  }
  // Only the draft's subject and body may be prefilled: no cc/bcc recipients.
  const auto items = QUrlQuery(candidate).queryItems();
  for (const auto &item : items) {
    if (item.first != QLatin1String("subject") && item.first != QLatin1String("body"))
      return false;
  }
  return true;
}

void wireEmailButton(QAbstractButton *button, QObject *context, EmailDraft draft,
                     UrlHandler handler) {
  if (!button || !context || !draft) return;
  QObject::connect(button, &QAbstractButton::clicked, context,
                   [draft = std::move(draft), handler = std::move(handler)] {
                     const QUrl target = draft();
                     if (handler && isAllowedEmail(target)) handler(target);
                   });
}

void wireLabel(QLabel *label, QObject *context, UrlHandler handler) {
  if (!label || !context) return;
  label->setTextInteractionFlags(Qt::TextBrowserInteraction);
  label->setOpenExternalLinks(false);
  QObject::connect(label, &QLabel::linkActivated, context,
                   [handler = std::move(handler)](const QString &link) {
                     dispatch(QUrl(link), handler);
                   });
}

}  // namespace dalinira::about_links
