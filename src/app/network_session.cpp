#include "app/network_session.h"

#include "app/application_controller.h"
#include "core/logger.h"
#include "network/peer_connection.h"
#include "protocol/packet.h"
#include "protocol/packet_codec.h"
#include "signaling/invitation_codec.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

using namespace tmc;

struct NetworkSession::Link {
    QString connectionId;
    PeerIdentity remote;
    std::shared_ptr<PeerConnection> transport;
    bool open{false};
    bool everOpened{false};
    bool localOffer{false};
    bool answerApplied{false};
    bool signalingProduced{false};
    bool meshManaged{false};
    ConnectionState state{ConnectionState::Disconnected};
};

static QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QJsonObject identityJson(const PeerIdentity& identity) {
    return {{"peer_id", identity.peerId},
            {"display_name", identity.displayName},
            {"device_id", identity.deviceId},
            {"created_at", identity.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
}

static PeerIdentity identityFromJson(const QJsonObject& object) {
    return {object.value("peer_id").toString(), object.value("display_name").toString(),
            object.value("device_id").toString(),
            QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs)};
}

NetworkSession::NetworkSession(ApplicationController& app, QObject* parent)
    : QObject(parent), app_(app) {
    qRegisterMetaType<ChatMessage>();
    keepalive_ = new QTimer(this);
    keepalive_->setInterval(app_.config().keepaliveSeconds * 1000);
    connect(keepalive_, &QTimer::timeout, this, [this] {
        for (const auto& link : links_)
            sendPacket(link,
                       basePacket("ping", {{"nonce", uuid()},
                                           {"sent_at", QDateTime::currentDateTimeUtc().toString(
                                                           Qt::ISODateWithMs)}}));
    });
    keepalive_->start();
}

NetworkSession::~NetworkSession() = default;

Result<void> NetworkSession::createRoom(const QString& name) {
    const auto normalized = name.trimmed();
    if (normalized.isEmpty())
        return Result<void>::failure("Введите название комнаты.");
    if (!links_.isEmpty())
        return Result<void>::failure(
            "Сначала завершите текущие соединения перезапуском приложения.");

    roomId_ = uuid();
    roomName_ = normalized;
    logicalClock_ = 0;
    peers_.clear();
    seenMessageIds_.clear();
    seenMessageOrder_.clear();
    delivery_ = {};
    rememberPeer(app_.identity());

    emit roomChanged(roomId_, roomName_);
    emit statusChanged("Комната создана. Приглашение может создать любой её участник.");
    return Result<void>::success();
}

Result<void> NetworkSession::createInvitation() {
    if (roomId_.isEmpty())
        return Result<void>::failure("Сначала создайте комнату.");
    if (knownPeerCount() >= app_.config().maxRoomPeers)
        return Result<void>::failure("Достигнут лимит участников комнаты.");

    auto link = makeLink(uuid());
    link->localOffer = true;
    emit statusChanged("Создаётся приглашение для нового участника…");
    try {
        link->transport->createOffer();
    } catch (const std::exception& e) {
        discardLink(link);
        return Result<void>::failure(QString::fromUtf8(e.what()));
    }
    std::weak_ptr<Link> weak = link;
    QTimer::singleShot(app_.config().iceGatheringTimeoutSeconds * 1000, this, [this, weak] {
        const auto pending = weak.lock();
        if (pending && !pending->signalingProduced)
            emit errorOccurred("Истёк таймаут сбора ICE-кандидатов. Проверьте STUN и сеть.");
    });
    return Result<void>::success();
}

Result<void> NetworkSession::importSignalingText(const QString& text) {
    auto decoded = InvitationCodec::decodeText(text.trimmed());
    if (!decoded)
        return Result<void>::failure(decoded.error());
    const auto document = InvitationCodec::encode(decoded.value());
    return importSignalingDocument(document);
}

Result<Invitation> NetworkSession::decodeSignaling(const QByteArray& document) const {
    return InvitationCodec::decode(document);
}

Result<void> NetworkSession::importSignalingDocument(const QByteArray& document) {
    auto decoded = decodeSignaling(document);
    if (!decoded)
        return Result<void>::failure(decoded.error());
    const auto invitation = decoded.value();

    if (invitation.kind == Invitation::Kind::Offer) {
        if (!roomId_.isEmpty() && roomId_ != invitation.roomId)
            return Result<void>::failure("Приглашение относится к другой комнате.");
        if (links_.contains(invitation.connectionId))
            return Result<void>::failure(
                "Повторно получен offer для уже известного соединения. Убедитесь, что друг "
                "отправил строку из окна «Answer готов», а не исходное приглашение.");

        for (const auto& existing : links_) {
            if (existing->remote.peerId == invitation.fromPeer.peerId && existing->open)
                return Result<void>::failure("Этот участник уже подключён к комнате.");
        }
        discardStaleLinks(invitation.fromPeer.peerId);

        const bool joiningRoom = roomId_.isEmpty();
        roomId_ = invitation.roomId;
        roomName_ = invitation.roomName;
        if (joiningRoom) {
            logicalClock_ = 0;
            peers_.clear();
            seenMessageIds_.clear();
            seenMessageOrder_.clear();
            delivery_ = {};
        }

        auto link = makeLink(invitation.connectionId);
        link->remote = invitation.fromPeer;
        rememberPeer(link->remote);
        rememberPeer(app_.identity());
        emit roomChanged(roomId_, roomName_);
        emit peerChanged(link->remote.peerId, link->remote.displayName, false);
        emit statusChanged("Offer импортирован. Создаётся answer…");
        try {
            link->transport->acceptOffer(invitation.sdp);
        } catch (const std::exception& e) {
            discardLink(link);
            return Result<void>::failure(QString::fromUtf8(e.what()));
        }
        return Result<void>::success();
    }

    if (roomId_ != invitation.roomId)
        return Result<void>::failure("Answer относится к другой комнате.");
    auto link = links_.value(invitation.connectionId);
    if (!link)
        return Result<void>::failure("Не найдено исходное приглашение для этого answer.");
    if (link->answerApplied)
        return Result<void>::failure("Этот answer уже импортирован.");

    link->remote = invitation.fromPeer;
    rememberPeer(link->remote);
    link->answerApplied = true;
    emit peerChanged(link->remote.peerId, link->remote.displayName, false);
    emit statusChanged("Answer импортирован. Устанавливается прямое P2P-соединение…");
    try {
        link->transport->acceptAnswer(invitation.sdp);
    } catch (const std::exception& e) {
        link->answerApplied = false;
        return Result<void>::failure(QString::fromUtf8(e.what()));
    }
    std::weak_ptr<Link> weak = link;
    QTimer::singleShot(app_.config().connectionTimeoutSeconds * 1000, this, [this, weak] {
        const auto pending = weak.lock();
        if (pending && !pending->open)
            emit errorOccurred("Не удалось установить прямое соединение за отведённое время.");
    });
    return Result<void>::success();
}

bool NetworkSession::rememberPeer(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || roomId_.isEmpty())
        return false;
    const bool known = peers_.contains(peer.peerId);
    auto current = peer;
    if (current.displayName.isEmpty())
        current.displayName = peer.peerId.left(8);
    if (current.deviceId.isEmpty())
        current.deviceId = "unknown";
    peers_.insert(peer.peerId, current);
    return !known;
}

QList<PeerIdentity> NetworkSession::knownPeers() const {
    return peers_.values();
}

bool NetworkSession::rememberMessage(const QString& messageId) {
    if (seenMessageIds_.contains(messageId))
        return false;
    constexpr qsizetype MaxSeenMessages = 4096;
    if (seenMessageOrder_.size() >= MaxSeenMessages) {
        const auto oldest = seenMessageOrder_.dequeue();
        seenMessageIds_.remove(oldest);
    }
    seenMessageIds_.insert(messageId);
    seenMessageOrder_.enqueue(messageId);
    return true;
}

std::shared_ptr<NetworkSession::Link> NetworkSession::makeLink(const QString& connectionId) {
    auto link = std::make_shared<Link>();
    link->connectionId = connectionId;
    link->transport = std::make_shared<PeerConnection>(app_.config());
    links_.insert(connectionId, link);
    configureLink(link);
    updateMesh();
    return link;
}

void NetworkSession::discardLink(const std::shared_ptr<Link>& link) {
    if (!link)
        return;
    const auto peerId = link->remote.peerId;
    const auto peerName = link->remote.displayName;
    links_.remove(link->connectionId);
    if (link->transport) {
        QObject::disconnect(link->transport.get(), nullptr, this, nullptr);
        link->transport.reset();
    }
    const auto replacement = linkForPeer(peerId);
    if (!peerId.isEmpty() && (!replacement || !replacement->open))
        emit peerChanged(peerId, peerName, false);
    updateMesh();
}

void NetworkSession::discardStaleLinks(const QString& peerId) {
    const auto snapshot = links_.values();
    for (const auto& link : snapshot) {
        const bool samePeer = !peerId.isEmpty() && link->remote.peerId == peerId;
        if (!link->open && (peerId.isEmpty() || samePeer))
            discardLink(link);
    }
    updateMesh();
}

void NetworkSession::configureLink(const std::shared_ptr<Link>& link) {
    connect(
        link->transport.get(), &PeerConnection::localDescriptionReady, this,
        [this, link](const QString& type, const QString& sdp) { emitSignaling(link, type, sdp); });
    connect(link->transport.get(), &PeerConnection::stateChanged, this,
            [this, link](ConnectionState state) {
                link->state = state;
                if (state == ConnectionState::Failed && link->open) {
                    link->open = false;
                    emit peerChanged(link->remote.peerId, link->remote.displayName, false);
                    updateMesh();
                }
                emit statusChanged("Соединение " + link->connectionId.left(8) + ": " +
                                   toString(state));
            });
    connect(link->transport.get(), &PeerConnection::channelOpened, this, [this, link] {
        link->open = true;
        link->everOpened = true;
        Logger::instance().log(QtInfoMsg, "network",
                               "Direct DataChannel opened for " + link->connectionId.left(8));
        emit peerChanged(link->remote.peerId, link->remote.displayName, true);
        emit statusChanged("Прямое P2P-соединение установлено. Relay не используется.");
        updateMesh();
        sendHello(link);
        sendPeerList(link);
        broadcastPeerList(link->connectionId);
        ensureDynamicMesh();
    });
    connect(link->transport.get(), &PeerConnection::channelClosed, this, [this, link] {
        link->open = false;
        emit peerChanged(link->remote.peerId, link->remote.displayName, false);
        emit statusChanged("Друг отключён.");
        updateMesh();
    });
    connect(link->transport.get(), &PeerConnection::textReceived, this,
            [this, link](const QString& text) { handleIncoming(link, text); });
    connect(link->transport.get(), &PeerConnection::errorOccurred, this,
            [this](const QString& error) { emit errorOccurred(error); });
}

void NetworkSession::emitSignaling(const std::shared_ptr<Link>& link, const QString& type,
                                   const QString& sdp) {
    Q_UNUSED(type)
    link->signalingProduced = true;
    const auto now = QDateTime::currentDateTimeUtc();
    Invitation invitation;
    invitation.kind = link->localOffer ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.roomId = roomId_;
    invitation.roomName = roomName_;
    invitation.connectionId = link->connectionId;
    invitation.sdp = sdp;
    invitation.nonce = uuid();
    invitation.fromPeer = app_.identity();
    invitation.createdAt = now;
    invitation.expiresAt = now.addSecs(10 * 60);

    if (link->meshManaged) {
        const auto packetType = link->localOffer ? "mesh.offer" : "mesh.answer";
        const auto phase = link->localOffer ? "offer" : "answer";
        broadcastService(packetType, {{"phase", phase},
                                      {"route_id", uuid()},
                                      {"hop_count", 0},
                                      {"connection_id", link->connectionId},
                                      {"from_peer", identityJson(app_.identity())},
                                      {"target_peer_id", link->remote.peerId},
                                      {"sdp", sdp}});
        emit statusChanged(link->localOffer ? "Mesh offer отправлен через доступные P2P-каналы."
                                            : "Mesh answer отправлен через доступные P2P-каналы.");
        return;
    }

    const auto document = InvitationCodec::encode(invitation);
    const auto text = InvitationCodec::encodeText(invitation);
    const auto kind = invitation.kind == Invitation::Kind::Offer ? "offer" : "answer";
    const auto extension = invitation.kind == Invitation::Kind::Offer ? ".tmcinvite" : ".tmcanswer";
    emit signalingReady(kind, text, document,
                        "tiny-mesh-" + link->connectionId.left(8) + extension);
    emit statusChanged(invitation.kind == Invitation::Kind::Offer
                           ? "Приглашение готово. Ожидание answer."
                           : "Answer готов. Отправьте его создателю комнаты.");
}

Packet NetworkSession::basePacket(const QString& type, const QJsonObject& payload) const {
    return {type,   uuid(), roomId_, app_.identity().peerId, QDateTime::currentDateTimeUtc(),
            payload};
}

void NetworkSession::sendPacket(const std::shared_ptr<Link>& link, const Packet& packet) {
    if (!link->open)
        return;
    const auto bytes = PacketCodec::encode(packet);
    if (!link->transport->sendText(QString::fromUtf8(bytes)))
        emit errorOccurred("Не удалось отправить пакет другу.");
}

void NetworkSession::sendHello(const std::shared_ptr<Link>& link) {
    sendPacket(link, basePacket("peer.hello", {{"display_name", app_.identity().displayName},
                                               {"device_id", app_.identity().deviceId}}));
}

void NetworkSession::handleIncoming(const std::shared_ptr<Link>& link, const QString& text) {
    QSet<QString> senders;
    if (!link->remote.peerId.isEmpty())
        senders.insert(link->remote.peerId);
    auto decoded = PacketCodec::decode(text.toUtf8(), roomId_, senders);
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "protocol", decoded.error());
        emit errorOccurred("Получен некорректный сетевой пакет: " + decoded.error());
        return;
    }
    handlePacket(link, decoded.value());
}

void NetworkSession::handlePacket(const std::shared_ptr<Link>& link, const Packet& packet) {
    if (packet.type == "peer.hello") {
        if (link->remote.peerId.isEmpty())
            link->remote.peerId = packet.senderId;
        const auto name = packet.payload.value("display_name").toString();
        if (!name.isEmpty())
            link->remote.displayName = name;
        const bool joined = rememberPeer(link->remote);
        emit peerChanged(link->remote.peerId, link->remote.displayName, true);
        sendPacket(link, basePacket("peer.hello_ack", {}));
        sendPeerList(link);
        if (joined)
            broadcastPeerList(link->connectionId);
        ensureDynamicMesh();
        return;
    }

    if (packet.type == "peer.list") {
        handlePeerList(link, packet);
        return;
    }

    if (packet.type == "chat.message") {
        const auto remoteClock = packet.payload.value("logical_clock").toInteger();
        logicalClock_ = qMax(logicalClock_, remoteClock) + 1;
        const auto now = QDateTime::currentDateTimeUtc();
        ChatMessage message{packet.payload.value("message_id").toString(),
                            roomId_,
                            packet.senderId,
                            packet.payload.value("text").toString(),
                            remoteClock,
                            packet.createdAt,
                            now};
        if (!message.isValid()) {
            emit errorOccurred("Получено некорректное сообщение.");
            return;
        }
        if (rememberMessage(message.messageId))
            emit messageReceived(message, false);
        sendPacket(link, basePacket("chat.ack", {{"message_id", message.messageId}}));
        return;
    }

    if (packet.type == "chat.ack") {
        const auto messageId = packet.payload.value("message_id").toString();
        if (delivery_.acknowledge(messageId, packet.senderId))
            emit deliveryChanged(messageId, delivery_.deliveredCount(messageId),
                                 delivery_.expectedCount(messageId));
        return;
    }

    if (packet.type == "mesh.offer") {
        handleMeshOffer(link, packet);
        return;
    }

    if (packet.type == "mesh.answer") {
        handleMeshAnswer(link, packet);
        return;
    }

    if (packet.type == "ping")
        sendPacket(link, basePacket("pong", packet.payload));
}

std::shared_ptr<NetworkSession::Link> NetworkSession::linkForPeer(const QString& peerId) const {
    std::shared_ptr<Link> fallback;
    for (const auto& link : links_) {
        if (link->remote.peerId != peerId)
            continue;
        if (link->open)
            return link;
        if (!fallback)
            fallback = link;
    }
    return fallback;
}

void NetworkSession::sendPeerList(const std::shared_ptr<Link>& link) {
    QJsonArray peers;
    for (const auto& peer : knownPeers())
        peers.append(identityJson(peer));
    sendPacket(link, basePacket("peer.list", {{"peers", peers}}));
}

void NetworkSession::broadcastPeerList(const QString& excludedConnection) {
    for (const auto& link : links_)
        if (link->open && link->connectionId != excludedConnection)
            sendPeerList(link);
}

void NetworkSession::handlePeerList(const std::shared_ptr<Link>& source, const Packet& packet) {
    bool changed = false;
    QSet<QString> knownIds;
    for (const auto& known : knownPeers())
        knownIds.insert(known.peerId);
    for (const auto& value : packet.payload.value("peers").toArray()) {
        const auto peer = identityFromJson(value.toObject());
        if (peer.peerId == app_.identity().peerId)
            continue;
        if (!knownIds.contains(peer.peerId) && knownIds.size() >= app_.config().maxRoomPeers)
            continue;
        if (rememberPeer(peer)) {
            changed = true;
            knownIds.insert(peer.peerId);
        }
        const auto direct = linkForPeer(peer.peerId);
        emit peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed)
        broadcastPeerList(source->connectionId);
    ensureDynamicMesh();
    updateMesh();
}

void NetworkSession::ensureDynamicMesh() {
    for (const auto& peer : knownPeers()) {
        if (peer.peerId == app_.identity().peerId ||
            app_.identity().peerId.compare(peer.peerId, Qt::CaseSensitive) > 0)
            continue;
        bool exists = false;
        const auto snapshot = links_.values();
        for (const auto& link : snapshot) {
            if (link->remote.peerId != peer.peerId)
                continue;
            if (link->open || !link->everOpened)
                exists = true;
            else
                discardLink(link);
        }
        if (exists)
            continue;
        auto mesh = makeLink(uuid());
        mesh->remote = peer;
        mesh->localOffer = true;
        mesh->meshManaged = true;
        emit statusChanged("Создаётся прямой канал с " + peer.displayName + "…");
        try {
            mesh->transport->createOffer();
        } catch (const std::exception& e) {
            discardLink(mesh);
            emit errorOccurred(QString::fromUtf8(e.what()));
        }
        std::weak_ptr<Link> weak = mesh;
        QTimer::singleShot(
            (app_.config().iceGatheringTimeoutSeconds + app_.config().connectionTimeoutSeconds) *
                1000,
            this, [this, weak] {
                const auto pending = weak.lock();
                if (pending && !pending->open && links_.contains(pending->connectionId)) {
                    emit errorOccurred("Не удалось автоматически построить прямой канал с " +
                                       pending->remote.displayName + ".");
                    discardLink(pending);
                }
            });
    }
}

void NetworkSession::broadcastService(const QString& type, QJsonObject payload,
                                      const QString& excludedConnection) {
    const auto routeId = payload.value("route_id").toString();
    rememberRoute(routeId);
    for (const auto& link : links_)
        if (link->open && link->connectionId != excludedConnection)
            sendPacket(link, basePacket(type, payload));
}

bool NetworkSession::rememberRoute(const QString& routeId) {
    if (seenRoutes_.contains(routeId))
        return false;
    if (seenRoutes_.size() >= 1024)
        seenRoutes_.clear();
    seenRoutes_.insert(routeId);
    return true;
}

void NetworkSession::handleMeshOffer(const std::shared_ptr<Link>& source, const Packet& packet) {
    auto payload = packet.payload;
    const auto routeId = payload.value("route_id").toString();
    if (!rememberRoute(routeId))
        return;
    if (payload.value("target_peer_id").toString() != app_.identity().peerId) {
        const auto hops = payload.value("hop_count").toInt();
        if (hops < app_.config().maxRoomPeers) {
            payload["hop_count"] = hops + 1;
            broadcastService("mesh.offer", payload, source->connectionId);
        }
        return;
    }

    const auto connectionId = payload.value("connection_id").toString();
    const auto origin = identityFromJson(payload.value("from_peer").toObject());
    const auto existing = linkForPeer(origin.peerId);
    if (links_.contains(connectionId) || (existing && (existing->open || !existing->everOpened)))
        return;
    if (existing)
        discardLink(existing);
    auto mesh = makeLink(connectionId);
    mesh->remote = origin;
    mesh->meshManaged = true;
    rememberPeer(origin);
    emit peerChanged(origin.peerId, origin.displayName, false);
    emit statusChanged("Получен автоматический offer от " + origin.displayName + "…");
    try {
        mesh->transport->acceptOffer(payload.value("sdp").toString());
    } catch (const std::exception& e) {
        discardLink(mesh);
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
    std::weak_ptr<Link> weak = mesh;
    QTimer::singleShot(
        (app_.config().iceGatheringTimeoutSeconds + app_.config().connectionTimeoutSeconds) * 1000,
        this, [this, weak] {
            const auto pending = weak.lock();
            if (pending && !pending->open && links_.contains(pending->connectionId)) {
                emit errorOccurred("Автоматический прямой канал не установился: " +
                                   pending->remote.displayName);
                discardLink(pending);
            }
        });
}

void NetworkSession::handleMeshAnswer(const std::shared_ptr<Link>& source, const Packet& packet) {
    auto payload = packet.payload;
    const auto routeId = payload.value("route_id").toString();
    if (!rememberRoute(routeId))
        return;
    if (payload.value("target_peer_id").toString() != app_.identity().peerId) {
        const auto hops = payload.value("hop_count").toInt();
        if (hops < app_.config().maxRoomPeers) {
            payload["hop_count"] = hops + 1;
            broadcastService("mesh.answer", payload, source->connectionId);
        }
        return;
    }

    const auto mesh = links_.value(payload.value("connection_id").toString());
    if (!mesh || !mesh->meshManaged || mesh->answerApplied)
        return;
    mesh->answerApplied = true;
    emit statusChanged("Получен mesh answer от " + mesh->remote.displayName + "…");
    try {
        mesh->transport->acceptAnswer(payload.value("sdp").toString());
    } catch (const std::exception& e) {
        mesh->answerApplied = false;
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
}

Result<void> NetworkSession::sendMessage(const QString& text) {
    const auto normalized = text.trimmed();
    if (roomId_.isEmpty())
        return Result<void>::failure("Сначала создайте комнату или импортируйте приглашение.");
    if (normalized.isEmpty())
        return Result<void>::failure("Сообщение пустое.");
    if (normalized.size() > PacketCodec::MaxTextChars)
        return Result<void>::failure("Максимальная длина сообщения — 4096 символов.");

    const auto now = QDateTime::currentDateTimeUtc();
    ChatMessage message{uuid(), roomId_, app_.identity().peerId, normalized, ++logicalClock_,
                        now,    now};
    rememberMessage(message.messageId);

    QSet<QString> targets;
    for (const auto& link : links_) {
        if (!link->open || link->remote.peerId.isEmpty())
            continue;
        targets.insert(link->remote.peerId);
    }
    delivery_.track(message.messageId, targets);
    emit messageReceived(message, true);
    emit deliveryChanged(message.messageId, 0, targets.size());

    const auto packet = basePacket("chat.message", {{"message_id", message.messageId},
                                                    {"text", message.text},
                                                    {"logical_clock", message.logicalClock}});
    for (const auto& link : links_)
        if (link->open)
            sendPacket(link, packet);
    return Result<void>::success();
}

Result<QPair<int, int>> NetworkSession::deliveryCounts(const QString& messageId) const {
    return Result<QPair<int, int>>::success(
        {delivery_.deliveredCount(messageId), delivery_.expectedCount(messageId)});
}

int NetworkSession::connectedPeerCount() const {
    QSet<QString> connected;
    for (const auto& link : links_)
        if (link->open && !link->remote.peerId.isEmpty())
            connected.insert(link->remote.peerId);
    return connected.size();
}

int NetworkSession::knownPeerCount() const {
    return knownPeers().size();
}

QString NetworkSession::diagnostics() const {
    QStringList lines{"Комната: " + (roomId_.isEmpty() ? QString("не выбрана") : roomName_),
                      "Room ID: " + (roomId_.isEmpty() ? QString("—") : roomId_),
                      QString("Прямых каналов: %1/%2")
                          .arg(connectedPeerCount())
                          .arg(qMax(0, knownPeerCount() - 1)),
                      "TURN/relay: отключён"};
    if (links_.isEmpty())
        lines.append("Соединения: отсутствуют");
    else
        lines.append("Соединения:");
    for (const auto& link : links_) {
        const auto peer =
            link->remote.displayName.isEmpty() ? "не определён" : link->remote.displayName;
        lines.append(
            QString("  %1 · %2 · %3").arg(peer, toString(link->state), link->connectionId.left(8)));
    }
    return lines.join('\n');
}

QString NetworkSession::peerDisplayName(const QString& peerId) const {
    const auto peer = peers_.value(peerId);
    if (!peer.displayName.isEmpty())
        return peer.displayName;
    return peerId.left(8);
}

void NetworkSession::updateMesh() {
    emit meshChanged(connectedPeerCount(), qMax(0, knownPeerCount() - 1));
}
