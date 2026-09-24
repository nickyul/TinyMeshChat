#include "tmc/signaling_client/signaling_client.h"
#include "tmc/signaling_protocol/authentication.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QAbstractSocket>
#include <QWebSocket>
#include <QHostAddress>

#include <algorithm>

namespace tmc {
using namespace signaling_protocol;
namespace {

constexpr int MaintenanceIntervalMs = 1000;
constexpr qint64 ConnectionAttemptTimeoutMs = 15000;
constexpr qint64 RequestTimeoutMs = 15000;
constexpr qint64 HeartbeatIntervalMs = 30000;
constexpr qint64 TurnRefreshRetryMs = 5000;
constexpr qint64 StableConnectionPeriodMs = 30000;
constexpr int InitialReconnectDelayMs = 1000;
constexpr int MaxReconnectDelayMs = 30000;
constexpr int MaxReconnectBackoffExponent = 5;
constexpr qsizetype MaxPendingRequests = 64;
constexpr qint64 MaxQueuedBytes = 256 * 1024;
constexpr qint64 OutgoingFrameOverheadAllowance = 16;

} // namespace

SignalingClient::SignalingClient(QObject* parent) : QObject(parent) {
    clock_.start();
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &SignalingClient::openSocket);
    maintenanceTimer_.setInterval(MaintenanceIntervalMs);
    connect(&maintenanceTimer_, &QTimer::timeout,
            this, &SignalingClient::maintainConnection);
}

SignalingClient::~SignalingClient() { resetSocket(); }

void SignalingClient::maintainConnection() {
    const auto now = clock_.elapsed();
    switch (state_) {
    case State::Connecting:
    case State::Authenticating:
        maintainConnectionAttempt(now);
        break;
    case State::Ready:
        maintainReadyConnection(now);
        break;
    case State::Disabled:
    case State::Reconnecting:
    case State::AccessRequired:
        break;
    }
}

void SignalingClient::maintainConnectionAttempt(qint64 now) {
    // One deadline covers socket opening and authentication.
    if (now - openedAt_ >= ConnectionAttemptTimeoutMs) {
        fail();
        return;
    }
    if (hasTimedOutRequest(now)) {
        fail();
        return;
    }
}

void SignalingClient::maintainReadyConnection(qint64 now) {
    if (now - readyAt_ >= StableConnectionPeriodMs) {
        retryAttempt_ = 0;
    }
    // TURN refresh is read-only and can be retried without resetting the room.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->type == "turn.refresh" && now - it->sentAt >= RequestTimeoutMs) {
            if (it.key() == turnRefreshRequestId_) {
                turnRefreshRequestId_.clear();
                turnRefreshAt_ = now + TurnRefreshRetryMs;
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (hasTimedOutRequest(now)) {
        fail();
        return;
    }
    maintainTurnCredentials(now);
    if (state_ == State::Ready) {
        maintainHeartbeat(now);
    }
}

TurnCredentials SignalingClient::turnCredentials() const {
    return ready() && clock_.elapsed() < turnExpiresAt_ ? turn_ : TurnCredentials{};
}

void SignalingClient::updateTurnCredentials(const QJsonObject& body) {
    // MessageCodec has already validated the object.
    turn_ = *decodeTurnCredentials(body.value("turn").toObject());
    const auto now = clock_.elapsed();
    turnExpiresAt_ = now + qint64{turn_.expiresInSeconds} * 1000;
    turnRefreshAt_ = turn_.urls.isEmpty() ? 0 : now + qint64{turn_.expiresInSeconds} * 500;
    turnRefreshRequestId_.clear();
    emit turnCredentialsChanged();
}

void SignalingClient::maintainTurnCredentials(qint64 now) {
    if (!turn_.urls.isEmpty() && now >= turnExpiresAt_) {
        turn_ = {};
        emit turnCredentialsChanged();
        if (!ready()) {
            return;
        }
    }
    if (turnRefreshAt_ == 0 || now < turnRefreshAt_ || !turnRefreshRequestId_.isEmpty()) {
        return;
    }
    turnRefreshAt_ = now + TurnRefreshRetryMs;
    const auto result = request("turn.refresh");
    if (const auto* id = std::get_if<QString>(&result)) {
        turnRefreshRequestId_ = *id;
    }
}

bool SignalingClient::hasTimedOutRequest(qint64 now) const {
    // A timeout leaves the outcome unknown; the caller closes the connection
    // without replaying requests that may already have changed server state.
    for (const auto& request : pending_) {
        if (now - request.sentAt >= RequestTimeoutMs) {
            return true;
        }
    }
    return false;
}

void SignalingClient::maintainHeartbeat(qint64 now) {
    if (now - pingAt_ < HeartbeatIntervalMs) {
        return;
    }
    if (!pendingPing_.isEmpty()) {
        fail();
        return;
    }
    pingAt_ = now;
    pendingPing_ = QByteArray::number(now);
    socket_->ping(pendingPing_);
}

bool SignalingClient::validServerUrl(const QUrl& url) {
    return url.isValid() && (url.scheme() == "ws" || url.scheme() == "wss") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() &&
           !url.hasFragment() && url.port() != 0 && url.toString().size() <= 2048 &&
           (url.scheme() == "wss" || url.host() == "localhost" || QHostAddress(url.host()).isLoopback());
}

void SignalingClient::resetSocket() {
    maintenanceTimer_.stop();
    pending_.clear();
    turn_ = {};
    turnExpiresAt_ = turnRefreshAt_ = 0;
    turnRefreshRequestId_.clear();
    pendingPing_.clear();
    sessionId_.clear();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        // This can run from a socket signal; destruction must wait until it returns.
        socket_.release()->deleteLater();
    }
}

void SignalingClient::stop() {
    if (state_ == State::Disabled) {
        return;
    }
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    retryAttempt_ = 0;
    const bool hadConnection = state_ != State::Disabled && state_ != State::Reconnecting && state_ != State::AccessRequired;
    resetSocket();
    state_ = State::Disabled;
    url_.clear();
    accessToken_.clear();
    expectedAuthority_.clear();
    if (hadConnection) {
        emit connectionLost();
    }
    if (state_ == State::Disabled) {
        emit stateChanged();
    }
}

void SignalingClient::fail() {
    if (state_ == State::Reconnecting ||
        state_ == State::Disabled ||
        state_ == State::AccessRequired) {
        return;
    }
    resetSocket();
    state_ = State::Reconnecting;
    if (reconnectEnabled_) {
        reconnectTimer_.start(std::min(MaxReconnectDelayMs, InitialReconnectDelayMs << retryAttempt_));
        retryAttempt_ = std::min(retryAttempt_ + 1, MaxReconnectBackoffExponent);
    }
    emit connectionLost();
    if (state_ == State::Reconnecting) {
        emit stateChanged();
    }
}

void SignalingClient::configureAccess(std::shared_ptr<security::SigningKey> key, QJsonObject grants) {
    signingKey_ = std::move(key);
    grants_ = std::move(grants);
}

void SignalingClient::redeemAccess(const QUrl& url, const QString& authority, const QString& token) {
    stop();
    if (state_ != State::Disabled) {
        return;
    }
    url_ = url;
    expectedAuthority_ = authority;
    accessToken_ = token;
    if (!validServerUrl(url) || !security::validKey(authority) || security::unbase64(token, 32).isEmpty()) {
        requireAccess("Некорректное приглашение доступа. Для внешнего сервера требуется wss://.");
        return;
    }
    // A one-use access invitation is not automatically replayed after an uncertain outcome.
    reconnectEnabled_ = true;
    openSocket();
}

void SignalingClient::requireAccess(const QString& reason) {
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    resetSocket();
    accessToken_.clear();
    expectedAuthority_.clear();
    state_ = State::AccessRequired;
    emit connectionLost();
    if (state_ != State::AccessRequired) {
        return;
    }
    emit stateChanged();
    if (state_ == State::AccessRequired) {
        emit accessRequired(reason);
    }
}

bool SignalingClient::connectTo(const QUrl& url) {
    if (!validServerUrl(url)) {
        return false;
    }
    stop();
    // A notification handler may have already started another connection.
    if (state_ != State::Disabled) {
        return true;
    }
    url_ = url;
    reconnectEnabled_ = true;
    openSocket();
    return true;
}

void SignalingClient::openSocket() {
    if (!reconnectEnabled_) return;
    socket_ = std::make_unique<QWebSocket>();
    socket_->setParent(this);
    socket_->setMaxAllowedIncomingFrameSize(EnvelopeCodec::MaxMessageBytes);
    socket_->setMaxAllowedIncomingMessageSize(EnvelopeCodec::MaxMessageBytes);
    connect(socket_.get(), &QWebSocket::textMessageReceived, this, &SignalingClient::receive);
    connect(socket_.get(), &QWebSocket::binaryMessageReceived, this, [this] { fail(); });
    connect(socket_.get(), &QWebSocket::disconnected, this, &SignalingClient::fail);
    connect(socket_.get(), &QWebSocket::errorOccurred, this, [this] { fail(); });
    connect(socket_.get(), &QWebSocket::pong, this, [this](quint64, const QByteArray& payload) {
        if (payload == pendingPing_) {
            pendingPing_.clear();
        }
    });
    nextRequest_ = 0;
    openedAt_ = pingAt_ = clock_.elapsed();
    state_ = State::Connecting;
    maintenanceTimer_.start();
    // TODO: Handle stop/reconnect from stateChanged before continuing with the socket.
    emit stateChanged();
    socket_->open(url_);
}

bool SignalingClient::ready() const { return state_ == State::Ready; }
QString SignalingClient::state() const {
    switch (state_) {
    case State::Disabled:
        return QStringLiteral("disabled");
    case State::Connecting:
        return QStringLiteral("connecting");
    case State::Authenticating:
        return QStringLiteral("authenticating");
    case State::Ready:
        return QStringLiteral("connected");
    case State::Reconnecting:
        return QStringLiteral("unavailable");
    case State::AccessRequired:
        return QStringLiteral("access-required");
    }
    Q_UNREACHABLE();
}
QUrl SignalingClient::url() const { return url_; }
QString SignalingClient::sessionId() const { return ready() ? sessionId_ : QString{}; }

SignalingClient::RequestResult SignalingClient::request(const QString& type, const QJsonObject& body) {
    const bool auth = state_ == State::Authenticating && (type == "auth.authenticate" || type == "access.redeem");
    if (!ready() && !auth) {
        return RequestError::NotReady;
    }
    if (pending_.size() >= MaxPendingRequests) {
        return RequestError::TooManyPendingRequests;
    }
    const auto id = (type == "turn.refresh" ? QStringLiteral("turn-") : QString{}) +
                    QString::number(++nextRequest_);
    const Envelope envelope{signaling_protocol::ProtocolVersion, type, id, body};
    if (MessageCodec::validateRequest(envelope)) {
        return RequestError::InvalidMessage;
    }
    const auto encoded = EnvelopeCodec::encode(envelope);
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    if (!bytes) {
        return std::get<CodecError>(encoded).code == CodecErrorCode::MessageTooLarge
            ? RequestError::MessageTooLarge : RequestError::InvalidMessage;
    }
    if (socket_->bytesToWrite() + bytes->size() + OutgoingFrameOverheadAllowance > MaxQueuedBytes) {
        return RequestError::OutgoingQueueFull;
    }
    pending_.insert(id, {type, clock_.elapsed()});
    if (socket_->sendTextMessage(QString::fromUtf8(*bytes)) < 0) {
        fail();
        return RequestError::SendFailed;
    }
    return id;
}

void SignalingClient::receive(const QString& text) {
    // TODO: A signal handler can restart the connection into the same state.
    const auto decoded = EnvelopeCodec::decode(text.toUtf8());
    const auto* message = std::get_if<Envelope>(&decoded);
    if (!message || MessageCodec::validateServerMessage(*message)) {
        fail();
        return;
    }

    if (message->type == "auth.challenge") {
        handleChallenge(*message);
        return;
    }

    if (!ready() && state_ != State::Authenticating) {
        fail();
        return;
    }

    if (!message->requestId) {
        if (!ready() || message->type == "error") {
            fail();
        } else {
            emit eventReceived(*message);
        }
        return;
    }

    handleResponse(*message);
}

void SignalingClient::handleChallenge(const Envelope& message) {
    if (state_ != State::Connecting) {
        fail();
        return;
    }

    sessionId_ = message.body.value("sessionId").toString();
    const auto authority = message.body.value("authority").toString();
    if (!signingKey_) {
        requireAccess("Не найден личный ключ.");
        return;
    }

    const AuthenticationChallenge challenge{
        sessionId_, message.body.value("nonce").toString(), authority};
    state_ = State::Authenticating;
    emit stateChanged();
    if (state_ != State::Authenticating) {
        return;
    }

    RequestResult result;
    if (!accessToken_.isEmpty()) {
        if (authority != expectedAuthority_) {
            requireAccess("Ключ сервера не совпадает с приглашением доступа.");
            return;
        }

        const auto token = accessToken_;
        accessToken_.clear();
        const auto signature =
            signingKey_->sign(challenge.redemptionMessage(signingKey_->publicKey(), token));
        result = request(
            "access.redeem",
            {{"publicKey", signingKey_->publicKey()},
             {"token", token},
             {"signature", signature}});
    } else {
        const auto grant = grants_.value(url_.toString(QUrl::FullyEncoded)).toObject();
        if (!security::verifyGrant(grant, authority, signingKey_->publicKey())) {
            requireAccess("Для этого сервера нужно отдельное разрешение доступа. "
                          "Приглашение в mesh его не предоставляет.");
            return;
        }

        const auto signature =
            signingKey_->sign(challenge.authenticationMessage(signingKey_->publicKey()));
        result = request(
            "auth.authenticate",
            {{"publicKey", signingKey_->publicKey()},
             {"grant", grant},
             {"signature", signature}});
    }

    if (const auto* error = std::get_if<RequestError>(&result)) {
        // SendFailed already schedules reconnect; it does not revoke access.
        if (*error != RequestError::SendFailed && state_ == State::Authenticating) {
            requireAccess("Не удалось подтвердить доступ к серверу.");
        }
    }
}

void SignalingClient::handleResponse(const Envelope& message) {
    const auto found = pending_.find(*message.requestId);
    if (found == pending_.end()) {
        // Recognize retired refresh replies without retaining an unbounded list
        // of timed-out request IDs. Never apply stale credentials to a new request.
        if (message.requestId->startsWith("turn-") &&
            (message.type == "turn.credentials" || message.type == "error")) {
            bool valid = false;
            const auto sequence = message.requestId->sliced(5).toULongLong(&valid);
            if (valid && sequence > 0 && sequence <= nextRequest_) return;
        }
        fail();
        return;
    }

    const auto type = found->type;
    pending_.erase(found);

    if (message.type == "error") {
        if (type == "turn.refresh") {
            turnRefreshRequestId_.clear();
            turnRefreshAt_ = clock_.elapsed() + TurnRefreshRetryMs;
            return;
        }
        if (type == "auth.authenticate" || type == "access.redeem") {
            if (message.body.value("code").toString() == "identity_in_use") {
                requireAccess("Этот профиль уже подключён к серверу. Отключите другую сессию "
                              "и подключитесь повторно.");
            } else {
                requireAccess("Доступ отклонён. Проверьте разрешение или получите новое "
                              "приглашение доступа.");
            }
            return;
        }
        emit requestFailed(*message.requestId, type, message.body.value("code").toString());
        return;
    }

    if (!MessageCodec::isResponseFor(type, message.type)) {
        fail();
        return;
    }

    if (type == "turn.refresh") {
        updateTurnCredentials(message.body);
        return;
    }
    if (type == "auth.authenticate" || type == "access.redeem") {
        handleAuthenticationResponse(type, message);
        return;
    }

    emit responseReceived(type, message);
}

void SignalingClient::handleAuthenticationResponse(const QString& requestType,
                                                 const Envelope& message) {
    QJsonObject grant;
    if (requestType == "access.redeem") {
        grant = message.body.value("grant").toObject();
        if (!security::verifyGrant(grant, expectedAuthority_, signingKey_->publicKey())) {
            requireAccess("Некорректное разрешение сервера.");
            return;
        }
        grants_.insert(url_.toString(QUrl::FullyEncoded), grant);
    }

    expectedAuthority_.clear();
    state_ = State::Ready;
    readyAt_ = clock_.elapsed();
    updateTurnCredentials(message.body);
    if (state_ != State::Ready) {
        return;
    }
    if (requestType == "access.redeem") {
        emit accessGranted(url_.toString(QUrl::FullyEncoded), grant);
    }
    if (state_ != State::Ready) {
        return;
    }
    emit stateChanged();
    if (state_ == State::Ready) {
        emit readyChanged();
    }
}

} // namespace tmc
