#pragma once

// Loopback HTTP fixture for blocker tests: deterministic, no Internet.

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cstdlib>
#include <functional>
#include <iostream>

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cerr << "CHECK FAILED " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
      std::exit(1);                                                                         \
    }                                                                                       \
  } while (false)

// Waits in the event loop (never spins) until `condition` holds.
inline bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return true;
}

struct FixtureRoute {
  int status = 200;
  QByteArray body;
  QByteArray contentType = "text/plain; charset=utf-8";
  QByteArray etag;
};

struct FixtureRequest {
  QByteArray method;
  QByteArray target;  // path and query, as sent
  QByteArray path;    // without the query
};

class HttpFixture final : public QObject {
 public:
  // Another loopback address (127.0.0.2) is another site to the engines.
  explicit HttpFixture(const QHostAddress &address = QHostAddress(QHostAddress::LocalHost)) {
    if (!server_.listen(address, 0)) std::exit(1);
    connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString url(const QString &target) const {
    return QStringLiteral("http://%1:%2%3").arg(server_.serverAddress().toString()).arg(server_.serverPort()).arg(target);
  }
  void setRoute(const QByteArray &path, const FixtureRoute &route) { routes_[path] = route; }
  void reset() {
    routes_.clear();
    requests.clear();
  }
  int count(const QByteArray &path, const QByteArray &method = {}) const {
    int total = 0;
    for (const FixtureRequest &request : requests)
      if (request.path == path && (method.isEmpty() || request.method == method)) ++total;
    return total;
  }
  QList<FixtureRequest> requests;

 private:
  void handle(QTcpSocket *socket) {
    QByteArray &buffer = buffers_[socket];
    buffer += socket->readAll();
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return;
    const QList<QByteArray> requestLine = buffer.left(buffer.indexOf("\r\n")).split(' ');
    buffers_.remove(socket);
    FixtureRequest request;
    request.method = requestLine.value(0);
    request.target = requestLine.value(1);
    request.path = request.target.left(request.target.indexOf('?') < 0 ? request.target.size()
                                                                        : request.target.indexOf('?'));
    requests.append(request);
    const FixtureRoute route = routes_.value(request.path, FixtureRoute{404, "missing", "text/plain", {}});
    QByteArray response = "HTTP/1.1 " + QByteArray::number(route.status) + " Fixture\r\n";
    response += "Content-Type: " + route.contentType + "\r\n";
    if (!route.etag.isEmpty()) response += "ETag: " + route.etag + "\r\n";
    response += "Content-Length: " + QByteArray::number(route.body.size()) + "\r\n";
    // Every request must reach the fixture: nothing may be answered from a cache.
    response += "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    socket->write(response + route.body);
    socket->disconnectFromHost();
  }

  QTcpServer server_;
  QHash<QByteArray, FixtureRoute> routes_;
  QHash<QTcpSocket *, QByteArray> buffers_;
};
