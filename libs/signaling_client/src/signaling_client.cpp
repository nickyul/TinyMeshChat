#include "tmc/signaling_client/signaling_client.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QAbstractSocket>
#include <QWebSocket>
#include <QHostAddress>

#include <algorithm>

namespace tmc {
using namespace signaling_protocol;

SignalingClient::SignalingClient(QObject* parent) : QObject(parent) {
    clock_.start();
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &SignalingClient::openSocket);
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        const auto now = clock_.elapsed();
        if (ready() && now - readyAt_ >= 30000) retryAttempt_ = 0;
        if ((state_ == "connecting" || state_ == "authenticating" || state_ == "authenticated") && now - openedAt_ >= 15000) {
            fail();
            return;
        }
        for (const auto& request : pending_) {
            if (now - request.sentAt >= 15000) {
                // The outcome may be unknown; never automatically replay mutations.
                fail();
                return;
            }
        }
        if (ready() && now - pingAt_ >= 30000) {
            if (!pendingPing_.isEmpty()) {
                fail();
                return;
            }
            pingAt_ = now;
            pendingPing_ = QByteArray::number(now);
            socket_->ping(pendingPing_);
        }
    });
}

SignalingClient::~SignalingClient() { resetSocket(); }

bool SignalingClient::validServerUrl(const QUrl& url) {
    return url.isValid() && (url.scheme() == "ws" || url.scheme() == "wss") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() &&
           !url.hasFragment() && url.port() != 0 && url.toString().size() <= 2048 &&
           (url.scheme() == "wss" || url.host() == "localhost" || QHostAddress(url.host()).isLoopback());
}

void SignalingClient::resetSocket() {
    timer_.stop();
    pending_.clear();
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
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    retryAttempt_ = 0;
    const bool hadConnection = state_ != "disabled" && state_ != "unavailable" && state_ != "access-required";
    resetSocket();
    state_ = QStringLiteral("disabled");
    url_.clear();
    accessToken_.clear();
    expectedAuthority_.clear();
    emit stateChanged();
    if (hadConnection) {
        emit connectionLost();
    }
}

void SignalingClient::fail() {
    if (state_ == "unavailable" || state_ == "disabled" || state_ == "access-required") {
        return;
    }
    resetSocket();
    state_ = QStringLiteral("unavailable");
    emit stateChanged();
    emit connectionLost();
    if (reconnectEnabled_ && state_ == "unavailable") {
        reconnectTimer_.start(std::min(30000, 1000 << retryAttempt_));
        retryAttempt_ = std::min(retryAttempt_ + 1, 5);
    }
}

void SignalingClient::configureAccess(std::shared_ptr<security::SigningKey> key, QJsonObject grants) {
    signingKey_ = std::move(key);
    grants_ = std::move(grants);
}

void SignalingClient::redeemAccess(const QUrl& url, const QString& authority, const QString& token) {
    stop();
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
    state_ = "access-required";
    emit connectionLost();
    emit stateChanged();
    emit accessRequired(reason);
}

void SignalingClient::connectTo(const QUrl& url) {
    stop();
    url_ = url;
    if (!validServerUrl(url)) {
        requireAccess("Некорректный адрес сервера. Для внешнего сервера требуется wss://; ws:// разрешён только для localhost.");
        return;
    }
    reconnectEnabled_ = true;
    openSocket();
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
    state_ = QStringLiteral("connecting");
    emit stateChanged();
    timer_.start();
    socket_->open(url_);
}

bool SignalingClient::ready() const { return state_ == "connected"; }
QString SignalingClient::state() const { return state_; }
QUrl SignalingClient::url() const { return url_; }
QString SignalingClient::sessionId() const { return ready() ? sessionId_ : QString{}; }

QString SignalingClient::request(const QString& type, const QJsonObject& body) {
    const bool auth = state_ == "authenticating" && (type == "auth.authenticate" || type == "access.redeem");
    if ((!ready() && !auth) || pending_.size() >= 64) {
        return {};
    }
    const auto id = QString::number(++nextRequest_);
    const Envelope envelope{1, type, id, body};
    if (MessageCodec::validateRequest(envelope)) {
        return {};
    }
    const auto encoded = EnvelopeCodec::encode(envelope);
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    if (!bytes || socket_->bytesToWrite() + bytes->size() + 16 > 256 * 1024) {
        return {};
    }
    pending_.insert(id, {type, clock_.elapsed()});
    if (socket_->sendTextMessage(QString::fromUtf8(*bytes)) < 0) {
        fail();
        return {};
    }
    return id;
}

void SignalingClient::receive(const QString& text) {
    const auto decoded = EnvelopeCodec::decode(text.toUtf8());
    const auto* message = std::get_if<Envelope>(&decoded);
    if (!message || MessageCodec::validateServerMessage(*message)) {
        fail();
        return;
    }
    if (message->type == "auth.challenge") {
        if (state_ != "connecting") {
            fail();
            return;
        }
        sessionId_ = message->body.value("sessionId").toString();
        const auto authority = message->body.value("authority").toString();
        if (!signingKey_) { requireAccess("Не найден личный ключ."); return; }
        QStringList fields{sessionId_, message->body.value("nonce").toString(), authority, signingKey_->publicKey()};
        state_ = "authenticating";
        emit stateChanged();
        QString id;
        if (!accessToken_.isEmpty()) {
            if (authority != expectedAuthority_) { requireAccess("Ключ сервера не совпадает с приглашением доступа."); return; }
            const auto token = accessToken_;
            accessToken_.clear();
            fields.append(token);
            id = request("access.redeem", {{"publicKey", signingKey_->publicKey()}, {"token", token},
                {"signature", signingKey_->sign(security::transcript("tmc.ws-redeem.v1", fields))}});
        } else {
            const auto grant = grants_.value(url_.toString(QUrl::FullyEncoded)).toObject();
            if (!security::verifyGrant(grant, authority, signingKey_->publicKey())) {
                requireAccess("Для этого сервера нужно отдельное разрешение доступа. Приглашение в mesh его не предоставляет."); return;
            }
            id = request("auth.authenticate", {{"publicKey", signingKey_->publicKey()}, {"grant", grant},
                {"signature", signingKey_->sign(security::transcript("tmc.ws-auth.v1", fields))}});
        }
        if (id.isEmpty()) requireAccess("Не удалось подтвердить доступ к серверу.");
        return;
    }
    if (message->type == "session.ready") {
        if (state_ != "authenticated" || message->body.value("sessionId").toString() != sessionId_) {
            requireAccess("Сервер не завершил проверку доступа.");
            return;
        }
        state_ = QStringLiteral("connected");
        sessionId_ = message->body.value("sessionId").toString();
        readyAt_ = clock_.elapsed();
        emit stateChanged();
        emit readyChanged();
        return;
    }
    if (!ready() && state_ != "authenticating") {
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
    const auto found = pending_.find(*message->requestId);
    if (found == pending_.end()) {
        fail();
        return;
    }
    const auto type = found->type;
    pending_.erase(found);
    if (message->type == "error") {
        if (type == "auth.authenticate" || type == "access.redeem") {
            requireAccess("Доступ отклонён. Проверьте разрешение или получите новое приглашение доступа.");
            return;
        }
        emit requestFailed(*message->requestId, type, message->body.value("code").toString());
        return;
    }
    static const QHash<QString, QString> responses{
        {"auth.authenticate", "auth.authenticated"}, {"access.redeem", "access.granted"},
        {"access.invite", "access.invited"},
        {"presence.publish", "presence.published"}, {"contact.invite", "contact.invited"},
        {"contact.respond", "contact.responded"}, {"room.create", "room.created"}, {"invite.create", "invite.created"},
        {"room.join", "room.joined"}, {"room.leave", "room.left"},
        {"signal.send", "signal.accepted"}};
    if (responses.value(type) != message->type) {
        fail();
        return;
    }
    if (type == "auth.authenticate" || type == "access.redeem") {
        if (type == "access.redeem") {
            const auto grant = message->body.value("grant").toObject();
            if (!security::verifyGrant(grant, expectedAuthority_, signingKey_->publicKey())) {
                requireAccess("Некорректное разрешение сервера."); return;
            }
            grants_.insert(url_.toString(QUrl::FullyEncoded), grant);
            emit accessGranted(url_.toString(QUrl::FullyEncoded), grant);
        }
        expectedAuthority_.clear();
        state_ = "authenticated";
        return;
    }
    emit responseReceived(type, *message);
}

} // namespace tmc
