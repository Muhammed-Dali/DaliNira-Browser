// DaliNira WebKit web process extension: network request filtering, and
// telling the browser which pages this process hosts.
//
// WebKitGTK loads this module into every web process of a DaliNira WebKit
// context. It hooks WebKitWebPage::send-request and asks the browser process
// what to do with each http(s) request. The browser answers with the same
// DaliNiraBlockerService decision the Chromium engine uses (rules, site
// policy, tracking-parameter stripping, HTTPS upgrade) and counts real blocks
// there, so the web process holds no filter lists and cannot fake counters.
//
// Protocol (one UNIX stream socket per web process, newline-terminated lines):
//   -> HELLO <token>
//   -> P <page-id>          this process now hosts the page
//   -> X <page-id>          the page left this process
//   -> Q <seq> <page-id> <method> <fetch-dest> <redirected> <url> <first-party-url>
//   <- <seq> A              allow
//   <- <seq> B              block
//   <- <seq> R <url>        continue with this URL (redirect / stripped URL)
// Fields are separated by tabs. URLs from WebKit are percent-encoded ASCII.
//
// If the browser cannot answer in time the request is allowed: page loads
// never hang on the filter, and the failure is logged once.
//
// P/X let the browser attribute memory to tabs (TabResourceModel): it takes
// this process's pid from the socket (SO_PEERCRED), not from the message.

#include <webkit2/webkit-web-extension.h>
#include "web_audio_latency.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr int kReplyTimeoutMs = 2000;
constexpr size_t kMaxUrlBytes = 64 * 1024;

struct FilterConnection {
  std::string socketPath;
  std::string token;
  int fd = -1;
  bool failed = false;
  unsigned long long nextSeq = 1;
  std::string readBuffer;
};

FilterConnection g_connection;

void reportFailureOnce(const char *what) {
  if (g_connection.failed) return;
  g_connection.failed = true;
  g_warning("DaliNira network filter unavailable (%s): %s; requests in this web process are not filtered",
            what, g_strerror(errno));
}

bool writeAll(int fd, const std::string &data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

bool ensureConnected() {
  if (g_connection.fd >= 0) return true;
  if (g_connection.failed || g_connection.socketPath.empty()) return false;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    reportFailureOnce("socket");
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (g_connection.socketPath.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    errno = ENAMETOOLONG;
    reportFailureOnce("socket path");
    return false;
  }
  std::memcpy(address.sun_path, g_connection.socketPath.c_str(), g_connection.socketPath.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      !writeAll(fd, "HELLO\t" + g_connection.token + "\n")) {
    ::close(fd);
    reportFailureOnce("connect");
    return false;
  }
  g_connection.fd = fd;
  return true;
}

void disconnect(const char *why) {
  if (g_connection.fd >= 0) ::close(g_connection.fd);
  g_connection.fd = -1;
  reportFailureOnce(why);
}

// Reads lines until the reply for `seq` arrives. Replies to earlier requests
// that timed out are discarded.
bool readReply(unsigned long long seq, std::string &reply) {
  const std::string prefix = std::to_string(seq) + "\t";
  gint64 deadline = g_get_monotonic_time() + static_cast<gint64>(kReplyTimeoutMs) * 1000;
  for (;;) {
    size_t newline;
    while ((newline = g_connection.readBuffer.find('\n')) != std::string::npos) {
      std::string line = g_connection.readBuffer.substr(0, newline);
      g_connection.readBuffer.erase(0, newline + 1);
      if (line.compare(0, prefix.size(), prefix) == 0) {
        reply = line.substr(prefix.size());
        return true;
      }
    }
    const gint64 remainingUs = deadline - g_get_monotonic_time();
    if (remainingUs <= 0) {
      g_warning("DaliNira network filter: no reply within %d ms; request allowed", kReplyTimeoutMs);
      return false;
    }
    pollfd pfd{g_connection.fd, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>((remainingUs + 999) / 1000));
    if (ready < 0) {
      if (errno == EINTR) continue;
      disconnect("poll");
      return false;
    }
    if (ready == 0) continue;
    char chunk[4096];
    const ssize_t n = ::recv(g_connection.fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      disconnect("connection closed");
      return false;
    }
    g_connection.readBuffer.append(chunk, static_cast<size_t>(n));
  }
}

bool isFilteredScheme(const char *uri) {
  return g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://");
}

bool isSafeField(const char *value) {
  if (!value) return true;
  const size_t length = std::strlen(value);
  return length <= kMaxUrlBytes && !std::strpbrk(value, "\t\r\n");
}

gboolean onSendRequest(WebKitWebPage *page, WebKitURIRequest *request, WebKitURIResponse *redirectedResponse,
                       gpointer) {
  const char *uri = webkit_uri_request_get_uri(request);
  if (!uri || !isFilteredScheme(uri) || !isSafeField(uri)) return FALSE;

  SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(request);
  // Global Privacy Control, as the Chromium engine sends it on every request.
  if (headers) soup_message_headers_replace(headers, "Sec-GPC", "1");
  const char *destination = headers ? soup_message_headers_get_one(headers, "Sec-Fetch-Dest") : nullptr;
  if (!destination || !isSafeField(destination)) destination = "";
  const char *method = webkit_uri_request_get_http_method(request);
  if (!method || !isSafeField(method)) method = "GET";
  // WebKitGTK does not consistently expose Sec-Fetch-Dest on the provisional
  // main-resource request.  At that point WebKitWebPage has already adopted
  // the provisional URI, whereas subframe and subresource requests leave the
  // page URI at the top-level document.  Recover the missing classification
  // so browser policy (notably HTTPS-first) sees the real main-frame request.
  const char *pageUri = webkit_web_page_get_uri(page);
  if (!*destination && pageUri && std::strcmp(uri, pageUri) == 0)
    destination = "document";
  // A main-frame navigation's first party is its own URL; everything else
  // belongs to the page's current top-level document.
  const char *firstParty = std::strcmp(destination, "document") == 0 ? uri : pageUri;
  if (!firstParty || !isSafeField(firstParty)) firstParty = "";

  if (!ensureConnected()) return FALSE;
  const unsigned long long seq = g_connection.nextSeq++;
  std::string query = "Q\t" + std::to_string(seq) + "\t" + std::to_string(webkit_web_page_get_id(page)) + "\t" +
                      method + "\t" + destination + "\t" + (redirectedResponse ? "1" : "0") + "\t" + uri + "\t" +
                      firstParty + "\n";
  if (!writeAll(g_connection.fd, query)) {
    disconnect("write");
    return FALSE;
  }
  std::string reply;
  if (!readReply(seq, reply)) return FALSE;
  if (reply == "B") return TRUE;  // Cancels the request.
  if (reply.compare(0, 2, "R\t") == 0 && reply.size() > 2) {
    webkit_uri_request_set_uri(request, reply.c_str() + 2);
  }
  return FALSE;
}

void announcePage(const char *kind, guint64 pageId) {
  // A page that leaves has nothing to tell a browser that is already gone
  // (e.g. at shutdown): no reconnect and no failure report for it.
  const bool leaving = kind[0] == 'X';
  if (leaving ? g_connection.fd < 0 : !ensureConnected()) return;
  if (writeAll(g_connection.fd, std::string(kind) + "\t" + std::to_string(pageId) + "\n")) return;
  if (!leaving) {
    disconnect("write");
    return;
  }
  ::close(g_connection.fd);
  g_connection.fd = -1;
}

// The page id travels in the weak-ref data pointer.
static_assert(sizeof(gsize) >= sizeof(guint64), "page ids must fit in a pointer");

void onPageFinalized(gpointer pageIdPointer, GObject *) {
  announcePage("X", GPOINTER_TO_SIZE(pageIdPointer));
}

void onPageCreated(WebKitWebExtension *, WebKitWebPage *page, gpointer) {
  g_signal_connect(page, "send-request", G_CALLBACK(onSendRequest), nullptr);
  const guint64 pageId = webkit_web_page_get_id(page);
  announcePage("P", pageId);
  g_object_weak_ref(G_OBJECT(page), onPageFinalized, GSIZE_TO_POINTER(pageId));
}

}  // namespace

extern "C" G_MODULE_EXPORT void webkit_web_extension_initialize_with_user_data(WebKitWebExtension *extension,
                                                                              GVariant *userData) {
  dalinira::engine::webkit::installWebAudioLatencyPolicy();
  const char *socketPath = nullptr;
  const char *token = nullptr;
  if (!userData || !g_variant_is_of_type(userData, G_VARIANT_TYPE_VARDICT) ||
      !g_variant_lookup(userData, "network-filter-socket", "&s", &socketPath) ||
      !g_variant_lookup(userData, "network-filter-token", "&s", &token)) {
    return;
  }
  g_connection.socketPath = socketPath;
  g_connection.token = token;
  g_signal_connect(extension, "page-created", G_CALLBACK(onPageCreated), nullptr);
}
