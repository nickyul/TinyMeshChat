#include "tmc/server/signaling_server.h"

#include "tmc/server/limits.h"
#include "tmc/signaling_protocol/authentication.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketProtocol>

#include <algorithm>

namespace tmc::server {
namespace {

using signaling_protocol::AuthenticationChallenge;
using signaling_protocol::CodecError;
using signaling_protocol::CodecErrorCode;
using signaling_protocol::Envelope;
using signaling_protocol::EnvelopeCodec;
using signaling_protocol::MessageCodec;

constexpr int MaxPendingConnections = 16;
constexpr int HandshakeTimeoutMs = 5000;
constexpr int MaintenanceIntervalMs = 1000;
constexpr int CloseHandshakeTimeoutMs = 5000;
constexpr qint64 AuthenticationTimeoutMs = 15000;
constexpr qsizetype MaxAccessInvitationsPerSession = 8;
constexpr qsizetype MaxAccessInvitations = 128;
constexpr qint64 OutgoingFrameOverheadAllowance = 16;
constexpr qint64 MaxQueuedBytes = 256 * 1024;
constexpr qint64 HeartbeatMilliseconds = 30 * 1000;

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

SignalingServer::SignalingServer(std::shared_ptr<security::SigningKey> authority,
                                 const std::optional<QSslConfiguration>& tls, QObject* parent)
    : QObject(parent),
      server_(QStringLiteral("TinyMesh Signaling Server"),
              tls ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode),
      authority_(std::move(authority)) {
    if (tls) {
        server_.setSslConfiguration(*tls);
    }
    clock_.start();
    server_.setMaxPendingConnections(MaxPendingConnections);
    server_.setHandshakeTimeout(HandshakeTimeoutMs);
    connect(&server_, &QWebSocketServer::newConnection,
            this, &SignalingServer::acceptPendingConnections);
    connect(&server_, &QWebSocketServer::acceptError, this,
            [](QAbstractSocket::SocketError) { qWarning("WebSocket accept error"); });
    connect(&server_, &QWebSocketServer::serverError, this,
            [](QWebSocketProtocol::CloseCode) { qWarning("WebSocket handshake error"); });
    maintenance_.setInterval(MaintenanceIntervalMs);
    connect(&maintenance_, &QTimer::timeout, this, &SignalingServer::maintainConnections);
}

SignalingServer::~SignalingServer() {
    close();
}

bool SignalingServer::listen(const QHostAddress& address, quint16 port) {
    if (!authority_ ||
        (server_.secureMode() == QWebSocketServer::NonSecureMode && !address.isLoopback())) {
        return false;
    }
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
    session.lastPingAt = clock_.elapsed();
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
    send(socket, {signaling_protocol::ProtocolVersion, QStringLiteral("auth.challenge"), std::nullopt,
                  {{"sessionId", sessionId},
                   {"nonce", session.nonce},
                   {"authority", authority_->publicKey()}}});
}

void SignalingServer::receive(QWebSocket* socket, const QString& text) {
    auto session = clients_.find(socket);
    if (session == clients_.end()) {
        return;
    }
    const auto now = clock_.elapsed();
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
    if (const auto failure = MessageCodec::validateRequest(request)) {
        send(socket, MessageCodec::error(request.requestId, failureCode(failure->code)));
        return;
    }
    const auto sessionId = session->id;
    if (handleAccess(socket, request)) {
        return;
    }
    session = clients_.find(socket);
    if (session == clients_.end()) {
        return;
    }
    if (session->identityId.isEmpty()) {
        send(socket, MessageCodec::error(request.requestId, "authentication_required"));
        closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated, "Authentication required");
        return;
    }
    if (request.type == "presence.publish" &&
        request.body.value("identityId").toString() != session->identityId) {
        send(socket, MessageCodec::error(request.requestId, "identity_mismatch"));
        return;
    }
    if (request.type.startsWith("presence.") || request.type.startsWith("contact.")) {
        deliver(presence_.handle(sessionId, request, rooms_, now));
    } else {
        auto deliveries = rooms_.handle(sessionId, request, now);
        if (request.type == "room.create" || request.type == "room.join" ||
            request.type == "room.leave") {
            deliveries += presence_.maintainInvitations(rooms_, now);
        }
        deliver(deliveries);
    }
}

bool SignalingServer::handleAccess(QWebSocket* socket, const Envelope& request) {
    const auto& type = request.type;
    if (!type.startsWith("auth.") && !type.startsWith("access.")) {
        return false;
    }

    auto session = clients_.find(socket);
    if (session == clients_.end()) {
        return true;
    }

    const auto reject = [&](const QString& code, bool close) {
        send(socket, MessageCodec::error(request.requestId, code));
        if (close) {
            closeClient(socket, QWebSocketProtocol::CloseCodePolicyViolated, "Access rejected");
        }
    };

    if (type == "access.invite") {
        if (session->identityId.isEmpty()) {
            reject("authentication_required", true);
            return true;
        }

        qsizetype count = 0;
        for (const auto& invite : accessInvitations_) {
            if (invite.issuerSession == session->id) {
                ++count;
            }
        }
        if (count >= MaxAccessInvitationsPerSession ||
            accessInvitations_.size() >= MaxAccessInvitations) {
            reject("resource_limit", false);
            return true;
        }

        const auto token = security::randomToken();
        const auto expiresAt = clock_.elapsed() +
                               signaling_protocol::InvitationLifetimeSeconds * qint64{1000};
        accessInvitations_.insert(token, {session->id, expiresAt});
        send(socket, {signaling_protocol::ProtocolVersion, "access.invited", request.requestId,
                      {{"token", token},
                       {"authority", authority_->publicKey()},
                       {"expiresInSeconds", signaling_protocol::InvitationLifetimeSeconds}}});
        return true;
    }

    if (!session->identityId.isEmpty() || session->nonce.isEmpty() ||
        clock_.elapsed() - session->openedAt >= AuthenticationTimeoutMs) {
        reject("authentication_failed", true);
        return true;
    }

    const AuthenticationChallenge challenge{session->id, session->nonce, authority_->publicKey()};
    session->nonce.clear(); // A proof is valid once and only on this socket.
    const auto publicKey = request.body.value("publicKey").toString();
    const auto signature = request.body.value("signature").toString();
    const auto identityId = security::identityId(publicKey);
    QString token;

    if (type == "auth.authenticate") {
        const auto grant = request.body.value("grant").toObject();
        if (!security::verifyGrant(grant, authority_->publicKey(), publicKey) ||
            !security::verify(publicKey, challenge.authenticationMessage(publicKey), signature)) {
            reject("authentication_failed", true);
            return true;
        }
    } else if (type == "access.redeem") {
        token = request.body.value("token").toString();
        const auto invitation = accessInvitations_.constFind(token);
        if (invitation == accessInvitations_.cend() || clock_.elapsed() >= invitation->expiresAt ||
            !security::verify(publicKey, challenge.redemptionMessage(publicKey, token), signature)) {
            reject("access_invitation_invalid", true);
            return true;
        }
    } else {
        reject("authentication_failed", true);
        return true;
    }

    // Check after proof verification, before consuming an invitation or granting access.
    const bool identityInUse = std::any_of(
        clients_.cbegin(), clients_.cend(),
        [&](const Session& other) { return other.identityId == identityId; });
    if (identityInUse) {
        reject("identity_in_use", true);
        return true;
    }

    session->identityId = identityId;
    if (type == "access.redeem") {
        accessInvitations_.remove(token);
        const auto grant = security::issueGrant(*authority_, publicKey);
        send(socket, {signaling_protocol::ProtocolVersion, "access.granted", request.requestId,
                      {{"grant", grant}}});
    } else {
        send(socket, {signaling_protocol::ProtocolVersion, "auth.authenticated", request.requestId,
                      {}});
    }
    return true;
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
    if (!bytes ||
        socket->bytesToWrite() + bytes->size() + OutgoingFrameOverheadAllowance > MaxQueuedBytes) {
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
        if (it->issuerSession == id) {
            it = accessInvitations_.erase(it);
        } else {
            ++it;
        }
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
    QTimer::singleShot(CloseHandshakeTimeoutMs, socket, [this, socket] {
        socket->abort();
        sockets_.remove(socket);
        socket->deleteLater();
    });
}

void SignalingServer::maintainConnections() {
    const auto now = clock_.elapsed();
    for (auto it = accessInvitations_.begin(); it != accessInvitations_.end();) {
        if (now >= it->expiresAt) {
            it = accessInvitations_.erase(it);
        } else {
            ++it;
        }
    }
    rooms_.expireInvitations(now);
    deliver(presence_.maintainInvitations(rooms_, now));
    const auto sockets = clients_.keys();
    for (auto* socket : sockets) {
        auto session = clients_.find(socket);
        if (session != clients_.end() && session->identityId.isEmpty() &&
            now - session->openedAt >= AuthenticationTimeoutMs) {
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
