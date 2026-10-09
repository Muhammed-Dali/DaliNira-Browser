#pragma once

#include <QString>
#include <QUrl>
#include <QUrlQuery>

// A dalinira://suggest command sent by the New Tab search field. Each engine
// validates the page capability in its own transport (Chromium
// acceptNavigationRequest, WebKit decide-policy); both then share this parser
// so the two engines accept exactly the same commands.
struct NewTabSuggestCommand {
  enum class Op { Invalid, Consent, Query, Activate, DeleteHistory, DeleteSuggestion };

  Op op = Op::Invalid;
  int id = -1;
  QString text;          // Query text, or the activated/deleted suggestion.
  QString input;         // DeleteHistory: field value to refresh afterwards.
  QString type;
  QUrl url;
  bool enabled = false;  // Consent.

  static NewTabSuggestCommand parse(const QUrl &url) {
    NewTabSuggestCommand command;
    if (url.scheme() != QLatin1String("dalinira") || url.host() != QLatin1String("suggest") ||
        url.toString().size() > 4096)
      return command;
    const QUrlQuery params(url);
    const QString op = params.queryItemValue(QStringLiteral("op"));
    bool idOk = false;
    const int id = params.queryItemValue(QStringLiteral("id")).toInt(&idOk);
    const QString q = params.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
    if (op == QLatin1String("consent")) {
      const QString value = params.queryItemValue(QStringLiteral("enabled"));
      if (value != QLatin1String("true") && value != QLatin1String("false")) return command;
      command.op = Op::Consent;
      command.enabled = value == QLatin1String("true");
    } else if (op == QLatin1String("query")) {
      if (!idOk || id < 0 || q.size() > 256) return command;
      command.op = Op::Query;
      command.id = id;
      command.text = q;
    } else if (op == QLatin1String("activate")) {
      command.text = q.simplified().left(256);
      if (command.text.isEmpty()) return command;
      command.op = Op::Activate;
    } else if (op == QLatin1String("delete-suggestion")) {
      command.type = params.queryItemValue(QStringLiteral("type"));
      command.url = QUrl(params.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded));
      command.text = q.simplified().left(256);
      if (!idOk || id < 0) return command;
      if (command.type == QLatin1String("search-history")) {
        if (command.text.isEmpty()) return command;
      } else if (command.type == QLatin1String("history") || command.type == QLatin1String("frequent")) {
        if (!command.url.isValid() || command.url.host().isEmpty() || !command.url.userInfo().isEmpty() ||
            (command.url.scheme() != QLatin1String("https") && command.url.scheme() != QLatin1String("http"))) return command;
      } else return command;
      command.op = Op::DeleteSuggestion;
      command.id = id;
      command.input = params.queryItemValue(QStringLiteral("input"), QUrl::FullyDecoded).left(256);
    } else if (op == QLatin1String("delete-history")) {
      command.text = q.simplified().left(256);
      if (!idOk || id < 0 || command.text.isEmpty()) return command;
      command.op = Op::DeleteHistory;
      command.id = id;
      command.input = params.queryItemValue(QStringLiteral("input"), QUrl::FullyDecoded).left(256);
    }
    return command;
  }
};
