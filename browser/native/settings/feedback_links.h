#pragma once

#include <QString>
#include <QUrl>

class QAbstractButton;
class QLabel;
class QObject;

#include <functional>

namespace dalinira::about_links {

enum class Destination {
  GitHubProfile,
  Repository,
  BugReport,
  FeatureRequest,
};

using UrlHandler = std::function<void(const QUrl &)>;

// These fixed URLs never receive the active tab URL, browser state, logs, or
// user data. isAllowed() requires an exact match, including query parameters.
QUrl url(Destination destination);
bool isAllowed(const QUrl &candidate);

// Shared wiring for every clickable item on Settings > About. The handler is
// SettingsPage's existing navigateRequested bridge in production and can be
// replaced with a recorder in tests without opening a real browser process.
void wireButton(QAbstractButton *button, Destination destination, QObject *context,
                UrlHandler handler);
void wireLabel(QLabel *label, QObject *context, UrlHandler handler);

// "Send Email" opens a draft in the user's own mail client; nothing is sent
// automatically. The draft is a mailto: URL to the fixed project address with
// only a subject and body, which callers fill with the localized template, the
// DaliNira version, and the engine name — never URLs, history, or profile data.
QString feedbackEmailAddress();
QUrl emailUrl(const QString &subject, const QString &body);
bool isAllowedEmail(const QUrl &candidate);
using EmailDraft = std::function<QUrl()>;
void wireEmailButton(QAbstractButton *button, QObject *context, EmailDraft draft,
                     UrlHandler handler);

}  // namespace dalinira::about_links
