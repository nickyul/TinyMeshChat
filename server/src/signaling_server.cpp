#include "signaling_server.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QWebSocket>
#include <QWebSocketProtocol>

namespace tmc::server {

namespace {

constexpr auto kProtocolUnavailable = "Signaling protocol is not implemented yet";
constexpr quint64 kMaxIncomingMessageSize = 64U * 1024U;

}  // namespace

SignalingServer::SignalingServer(QObject* parent)
    : QObject(parent),
      server_(QStringLiteral("TinyMesh Signaling Server"),
              QWebSocketServer::NonSecureMode) {
    connect(&server_, &QWebSocketServer::newConnection,
            this, &SignalingServer::acceptPendingConnections);
    connect(&server_, &QWebSocketServer::acceptError, this,
            [this](QAbstractSocket::SocketError) {
                qWarning().noquote()
                    << QStringLiteral("WebSocket accept error: %1")
                           .arg(server_.errorString());
            });
    connect(&server_, &QWebSocketServer::serverError, this,
            [this](QWebSocketProtocol::CloseCode) {
                qWarning().noquote()
                    << QStringLiteral("WebSocket handshake error: %1")
                           .arg(server_.errorString());
            });
}

SignalingServer::~SignalingServer() {
    close();
}

bool SignalingServer::listen(const QHostAddress& address, quint16 port) {
    return server_.listen(address, port);
}

void SignalingServer::close() {
    server_.close();

    const auto clients = clients_;
    for (QWebSocket* client : clients) {
        client->close(QWebSocketProtocol::CloseCodeGoingAway,
                      QStringLiteral("Server is shutting down"));
    }
}

QString SignalingServer::errorString() const {
    return server_.errorString();
}

QHostAddress SignalingServer::serverAddress() const {
    return server_.serverAddress();
}

quint16 SignalingServer::serverPort() const {
    return server_.serverPort();
}

void SignalingServer::acceptPendingConnections() {
    while (server_.hasPendingConnections()) {
        if (QWebSocket* socket = server_.nextPendingConnection()) {
            registerClient(socket);
        }
    }
}

void SignalingServer::registerClient(QWebSocket* socket) {
    socket->setParent(this);
    socket->setMaxAllowedIncomingFrameSize(kMaxIncomingMessageSize);
    socket->setMaxAllowedIncomingMessageSize(kMaxIncomingMessageSize);
    clients_.insert(socket);

    qInfo().noquote()
        << QStringLiteral("WebSocket connected: %1:%2")
               .arg(socket->peerAddress().toString())
               .arg(socket->peerPort());

    connect(socket, &QWebSocket::textMessageReceived, socket,
            [socket](const QString&) {
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                              QString::fromLatin1(kProtocolUnavailable));
            });
    connect(socket, &QWebSocket::binaryMessageReceived, socket,
            [socket](const QByteArray&) {
                socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                              QStringLiteral("Binary messages are not supported"));
            });
    connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        qInfo().noquote()
            << QStringLiteral("WebSocket disconnected: %1:%2")
                   .arg(socket->peerAddress().toString())
                   .arg(socket->peerPort());
        clients_.remove(socket);
        socket->deleteLater();
    });
}

}  // namespace tmc::server
