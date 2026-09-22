#include "signaling_server.h"

#include "tmc/signaling_protocol/message_codec.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketProtocol>

#include <algorithm>

namespace tmc::server {
namespace {

using signaling_protocol::CodecError;
using signaling_protocol::CodecErrorCode;
using signaling_protocol::Envelope;
using signaling_protocol::EnvelopeCodec;
using signaling_protocol::MessageCodec;

constexpr qsizetype MaxSessions = 64;
constexpr qsizetype MaxRequestsPerSession = 4096;
constexpr qint64 MaxQueuedBytes = 256 * 1024;
constexpr qint64 HeartbeatMilliseconds = 30 * 1000;
constexpr double RequestsPerSecond = 20;
constexpr double RequestBurst = 40;

QString failureCode(CodecErrorCode code) {
    switch (code) {
    case CodecErrorCode::UnsupportedVersion:
        return QStringLiteral("unsupported_version");
    case CodecErrorCode::UnknownType:
        return QStringLiteral("unknown_type");
    case CodecErrorCode::MessageTooLarge:
        return QStringLiteral("message_too_large");
    default:
        return QStringLiteral("invalid_message");
    }
}

} // namespace

SignalingServer::SignalingServer(std::shared_ptr<security::SigningKey> authority, QObject* parent)
    : QObject(parent),
      server_(QStringLiteral("TinyMesh Signaling Server"), QWebSocketServer::NonSecureMode), authority_(std::move(authority)) {
    clock_.start();
    server_.setMaxPendingConnections(16);
    server_.setHandshakeTimeout(5000);
    connect(&server_, &QWebSocketServer::newConnection,
            this, &SignalingServer::acceptPendingConnections);
    connect(&server_, &QWebSocketServer::acceptError, this,
            [](QAbstractSocket::SocketError) { qWarning("WebSocket accept error"); });
    connect(&server_, &QWebSocketServer::serverError, this,
            [](QWebSocketProtocol::CloseCode) { qWarning("WebSocket handshake error"); });
    maintenance_.setInterval(1000);
    connect(&maintenance_, &QTimer::timeout, this, &SignalingServer::maintainConnections);
}

SignalingServer::~SignalingServer() {
    close();
}

bool SignalingServer::listen(const QHostAddress& address, quint16 port) {
    if (!authority_ || !address.isLoopback()) return false;
    if (!server_.listen(address, port)) {
        return false;
    }
    maintenance_.start();
    return true;
}

void SignalingServer::close() {
    maintenance_.stop();
    server_.close();
    const auto sockets = sockets_;
    clients_.clear();
    pendingDeliveries_.clear();
    rooms_.clear();
    presence_.clear();
    accessInvitations_.clear();
    for (auto* socket : sockets) {
        closeClient(socket, QWebSocketProtocol::CloseCodeGoingAway,
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
        if (auto* socket = server_.nextPendingConnection()) {
            registerClient(socket);
        }
    }
}

void SignalingServer::registerClient(QWebSocket* socket) {
    socket->setParent(this);
    socket->setMaxAllowedIncomingFrameSize(EnvelopeCodec::MaxMessageBytes);
    socket->setMaxAllowedIncomingMessageSize(EnvelopeCodec::MaxMessageBytes);
    connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        removeClient(socket);
        sockets_.remove(socket);
        socket->deleteLater();
    });
    if (sockets_.size() >= MaxSessions) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    sockets_.insert(socket);
    Session session;
    do {
        session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (socketForSession(session.id));
    session.budgetUpdatedAt = session.lastPingAt = clock_.elapsed();
    session.openedAt = clock_.elapsed();
    session.nonce = security::randomToken();
    const auto sessionId = session.id;
    clients_.insert(socket, session);

    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, socket](const QString& text) { receive(socket, text); });
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this, socket](const QByteArray&) {
                closeClient(socket, QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                            QStringLiteral("Binary messages are not supported"));
            });
    connect(socket, &QWebSocket::pong, this,
            [this, socket](quint64, const QByteArray& payload) {
                auto session = clients_.find(socket);
                if (session != clients_.end() && !session->pendingPing.isEmpty() &&
                    session->pendingPing == payload) {
                    session->pendingPing.clear();
                }
            });
    send(socket, {1, QStringLiteral("auth.challenge"), std::nullopt,
                  {{"sessionId", sessionId}, {"nonce", session.nonce}, {"authority", authority_->publicKey()}}});
}

void SignalingServer::receive(QWebSocket* socket, const QString& text) {
    auto session = clients_.find(socket);
    if (session == clients_.end()) {
        return;
    }
    const auto now = clock_.elapsed();
    session->requestBudget = std::min(RequestBurst,
        session->requestBudget + (now - session->budgetUpdatedAt) * RequestsPerSecond / 1000.0);
    session->budgetUpdatedAt = now;
    if (session->requestBudget < 1) {
        send(socket, MessageCodec::error(std::nullopt, QStringLiteral("rate_limited")));
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("Request rate limit exceeded"));
        return;
    }
    session->requestBudget -= 1;
    const auto decoded = EnvelopeCodec::decode(text.toUtf8());
    if (const auto* failure = std::get_if<CodecError>(&decoded)) {
        send(socket, MessageCodec::error(std::nullopt, failureCode(failure->code)));
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("Invalid protocol envelope"));
        return;
    }
    const auto& request = std::get<Envelope>(decoded);
    if (!request.requestId) {
        send(socket, MessageCodec::error(std::nullopt, QStringLiteral("invalid_message")));
        return;
    }
    if (session->requestIds.contains(*request.requestId)) {
        send(socket, MessageCodec::error(request.requestId, QStringLiteral("replayed_request")));
        return;
    }
    if (session->requestIds.size() >= MaxRequestsPerSession) {
        send(socket, MessageCodec::error(request.requestId, QStringLiteral("resource_limit")));
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("Request history limit reached; open a new session"));
        return;
    }
    session->requestIds.insert(*request.requestId);
    if (const auto failure = MessageCodec::validateRequest(request)) {
        send(socket, MessageCodec::error(request.requestId, failureCode(failure->code)));
        return;
    }
    const auto sessionId = session->id;
    if (handleAccess(socket, request)) return;
    session = clients_.find(socket);
    if (session == clients_.end()) return;
    if (session->identityId.isEmpty()) {
        send(socket, MessageCodec::error(request.requestId, "authentication_required"));
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated, "Authentication required");
        return;
    }
    if (request.type == "presence.publish" && request.body.value("identityId").toString() != session->identityId) {
        send(socket, MessageCodec::error(request.requestId, "identity_mismatch"));
        return;
    }
    if (request.type.startsWith("presence.") || request.type.startsWith("contact.")) {
        deliver(presence_.handle(sessionId, request, rooms_, now));
    } else {
        auto deliveries = rooms_.handle(sessionId, request, now);
        deliveries += presence_.maintain(rooms_, now);
        deliver(deliveries);
    }
}

bool SignalingServer::send(QWebSocket* socket, const Envelope& message) {
    if (!socket || !clients_.contains(socket) ||
        socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    if (MessageCodec::validateServerMessage(message)) {
        qWarning("Invalid outgoing signaling message");
        closeClient(socket, QWebSocketProtocol::CloseCodeBadOperation,
                    QStringLiteral("Internal protocol error"));
        return false;
    }
    const auto encoded = EnvelopeCodec::encode(message);
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    if (!bytes || socket->bytesToWrite() + bytes->size() + 16 > MaxQueuedBytes) {
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated,
                    QStringLiteral("Outgoing queue limit exceeded"));
        return false;
    }
    if (socket->sendTextMessage(QString::fromUtf8(*bytes)) < 0) {
        closeClient(socket, QWebSocketProtocol::CloseCodeGoingAway,
                    QStringLiteral("Cannot queue outgoing message"));
        return false;
    }
    return true;
}

void SignalingServer::deliver(const QVector<Delivery>& deliveries) {
    if (deliveries.isEmpty()) {
        return;
    }
    pendingDeliveries_.enqueue(deliveries);
    if (delivering_) {
        return;
    }
    // Closing a slow recipient can synchronously produce disconnect events.
    // Finish the current batch before those events, preserving join/leave order.
    delivering_ = true;
    while (!pendingDeliveries_.isEmpty()) {
        const auto batch = pendingDeliveries_.dequeue();
        bool relayFailed = false;
        for (const auto& delivery : batch) {
            auto* socket = socketForSession(delivery.sessionId);
            if (delivery.message.type == "signal.accepted" && relayFailed) {
                send(socket, MessageCodec::error(delivery.message.requestId,
                                                 QStringLiteral("recipient_unavailable")));
            } else {
                const bool sent = send(socket, delivery.message);
                if (delivery.message.type == "signal.received" && !sent) {
                    relayFailed = true;
                }
            }
        }
    }
    delivering_ = false;
}

QWebSocket* SignalingServer::socketForSession(const QString& sessionId) const {
    for (auto session = clients_.cbegin(); session != clients_.cend(); ++session) {
        if (session->id == sessionId) {
            return session.key();
        }
    }
    return nullptr;
}

void SignalingServer::removeClient(QWebSocket* socket) {
    auto session = clients_.find(socket);
    if (session == clients_.end()) {
        return;
    }
    const auto id = session->id;
    clients_.erase(session);
    for (auto it = accessInvitations_.begin(); it != accessInvitations_.end();) {
        if (it->issuerSession == id) it = accessInvitations_.erase(it); else ++it;
    }
    auto deliveries = rooms_.disconnect(id);
    deliveries += presence_.disconnect(id, rooms_, clock_.elapsed());
    deliver(deliveries);
}

void SignalingServer::closeClient(QWebSocket* socket, QWebSocketProtocol::CloseCode code,
                                 const QString& reason) {
    removeClient(socket);
    socket->close(code, reason);
    // Bound closing sockets even when a peer never completes the close handshake.
    QTimer::singleShot(5000, socket, [this, socket] {
        socket->abort();
        sockets_.remove(socket);
        socket->deleteLater();
    });
}

void SignalingServer::maintainConnections() {
    const auto now = clock_.elapsed();
    for (auto it = accessInvitations_.begin(); it != accessInvitations_.end();) {
        if (now >= it->expiresAt) it = accessInvitations_.erase(it); else ++it;
    }
    rooms_.expireInvitations(now);
    deliver(presence_.maintain(rooms_, now));
    const auto sockets = clients_.keys();
    for (auto* socket : sockets) {
        auto session = clients_.find(socket);
        if (session != clients_.end() && session->identityId.isEmpty() && now - session->openedAt >= 15000) {
            closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated, "Authentication timeout");
            continue;
        }
        if (session == clients_.end() || now - session->lastPingAt < HeartbeatMilliseconds) {
            continue;
        }
        if (!session->pendingPing.isEmpty()) {
            closeClient(socket, QWebSocketProtocol::CloseCodeGoingAway,
                        QStringLiteral("Heartbeat timeout"));
            continue;
        }
        session->lastPingAt = now;
        session->pendingPing = QByteArray::number(now);
        socket->ping(session->pendingPing);
    }
}

} // namespace tmc::server
