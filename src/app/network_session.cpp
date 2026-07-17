#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"
#include "tmc/protocol/packet.h"
#include "tmc/protocol/packet_codec.h"
#include "tmc/signaling/invitation_codec.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace tmc {

namespace {

QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject identityJson(const PeerIdentity& identity) {
    return {{"peer_id", identity.peerId},
            {"display_name", identity.displayName},
            {"device_id", identity.deviceId},
            {"created_at", identity.createdAt.toUTC().toString(Qt::ISODateWithMs)}};
}

PeerIdentity identityFromJson(const QJsonObject& object) {
    return {object.value("peer_id").toString(), object.value("display_name").toString(),
            object.value("device_id").toString(),
            QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs)};
}

} // namespace

NetworkSession::NetworkSession(ApplicationController& app, ConnectionPolicy policy, QObject* parent)
    : QObject(parent), app_(app), policy_(policy),
      connections_(std::make_unique<ConnectionManager>(app.config().stunServers, policy)),
      mesh_(policy), voice_(std::make_unique<VoiceSession>()) {
    Q_ASSERT(policy_.isValid());
    qRegisterMetaType<ChatMessage>();
    qRegisterMetaType<ConnectionAttemptState>();
    qRegisterMetaType<MeshSessionState>();

    connect(connections_.get(), &ConnectionManager::localDescriptionReady, this,
            &NetworkSession::emitSignaling);
    connect(connections_.get(), &ConnectionManager::statusChanged, this,
            &NetworkSession::statusChanged);
    connect(connections_.get(), &ConnectionManager::attemptChanged, this,
            &NetworkSession::connectionAttemptChanged);
    connect(connections_.get(), &ConnectionManager::attemptFailed, this,
            [this](const PeerIdentity& peer, bool meshManaged, bool localOffer, const QString&) {
                if (meshManaged && localOffer) {
                    mesh_.scheduleRetry(peer);
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::linkOpened, this,
            [this](const QString& connectionId, const PeerIdentity& remote) {
                mesh_.connectionOpened(remote);
                Logger::instance().log(QtInfoMsg, "network",
                                       "Direct DataChannel opened for " + connectionId.left(8));
                emit peerChanged(remote.peerId, remote.displayName, true);
                emit statusChanged("Прямое P2P-соединение установлено. Relay не используется.");
                updateMesh();
                sendHello(connectionId);
                sendPeerList(connectionId);
                sendVoiceState(connectionId);
                broadcastPeerList(connectionId);
                ensureDynamicMesh();
            });
    connect(connections_.get(), &ConnectionManager::linkRemoved, this,
            [this](const QString&, const PeerIdentity& remote, bool wasOpen) {
                const auto replacement = connections_->infoForPeer(remote.peerId);
                if (!remote.peerId.isEmpty() && (!replacement || !replacement->open)) {
                    emit peerChanged(remote.peerId, remote.displayName, false);
                    voice_->removePeer(remote.peerId);
                }
                if (wasOpen) {
                    emit statusChanged("Участник отключился от прямого P2P-канала.");
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::textReceived, this,
            &NetworkSession::handleIncoming);
    connect(connections_.get(), &ConnectionManager::voiceFrameReceived, this,
            [this](const QString& connectionId, quint32 sequence, const QByteArray& payload) {
                const auto connection = connections_->info(connectionId);
                if (connection) {
                    voice_->receiveFrame(connection->remote.peerId, sequence, payload);
                }
            });

    connect(&mesh_, &MeshCoordinator::stateChanged, this, &NetworkSession::meshStateChanged);
    connect(&mesh_, &MeshCoordinator::statusChanged, this, &NetworkSession::statusChanged);
    connect(&mesh_, &MeshCoordinator::retryRequested, this, &NetworkSession::startMeshOffer);

    connect(voice_.get(), &VoiceSession::encodedFrameReady, this,
            [this](quint32 sequence, const QByteArray& payload) {
                if (voice_->active() && !voice_->muted()) {
                    connections_->sendVoiceFrameToOpen(sequence, payload);
                }
            });
    connect(voice_.get(), &VoiceSession::stateChanged, this, [this](bool active, bool muted) {
        broadcastVoiceState();
        emit callStateChanged(active, muted);
    });
    connect(voice_.get(), &VoiceSession::peerChanged, this, &NetworkSession::peerVoiceChanged);
    connect(voice_.get(), &VoiceSession::errorOccurred, this, &NetworkSession::errorOccurred);

    keepalive_ = new QTimer(this);
    keepalive_->setInterval(policy_.heartbeatIntervalSeconds * 1000);
    connect(keepalive_, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto inactive =
            connections_->inactiveConnectionIds(now, policy_.livenessTimeoutSeconds * 1000LL);
        for (const auto& connectionId : inactive) {
            const auto connection = connections_->info(connectionId);
            if (connection) {
                emit statusChanged("Соединение с участником потеряно: " +
                                   peerDisplayName(connection->remote.peerId));
            }
            connections_->discard(connectionId);
        }
        for (const auto& connectionId : connections_->openConnectionIds()) {
            sendPacket(connectionId,
                       basePacket("ping", {{"nonce", uuid()},
                                           {"sent_at", QDateTime::currentDateTimeUtc().toString(
                                                           Qt::ISODateWithMs)}}));
        }
    });
    keepalive_->start();

    connect(&app_, &ApplicationController::displayNameChanged, this, [this] {
        if (mesh_.meshId().isEmpty()) {
            return;
        }
        mesh_.rememberPeer(app_.identity());
        for (const auto& connectionId : connections_->openConnectionIds()) {
            sendHello(connectionId);
        }
        broadcastPeerList();
    });
    connect(&app_, &ApplicationController::stunServersChanged, connections_.get(),
            &ConnectionManager::setStunServers);
}

NetworkSession::~NetworkSession() = default;

MeshSessionState NetworkSession::meshState() const {
    return mesh_.state();
}

Result<void> NetworkSession::createMesh() {
    if (!connections_->connections().isEmpty() || !mesh_.meshId().isEmpty()) {
        return Result<void>::failure("Сначала покиньте текущую mesh-сессию.");
    }
    clearSessionData();
    mesh_.create(app_.identity(), uuid());
    emit statusChanged("Mesh создан. Теперь можно пригласить участника.");
    updateMesh();
    return Result<void>::success();
}

void NetworkSession::leaveMesh() {
    if (mesh_.meshId().isEmpty() && connections_->connections().isEmpty()) {
        return;
    }
    voice_->leave();
    const auto connections = connections_->connections();
    for (const auto& connection : connections) {
        connections_->discard(connection.connectionId);
    }
    mesh_.leave();
    clearSessionData();
    emit statusChanged("Вы вышли из mesh.");
    updateMesh();
}

Result<void> NetworkSession::createInvitation() {
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала создайте mesh или завершите подключение.");
    }
    if (knownPeerCount() >= policy_.maxPeers) {
        return Result<void>::failure("Достигнут лимит участников mesh.");
    }

    const auto connectionId = uuid();
    auto created = connections_->create(connectionId, {}, true, false);
    if (!created) {
        return created;
    }
    emit statusChanged("Создаётся приглашение для нового участника…");
    return connections_->startOffer(connectionId);
}

Result<void> NetworkSession::importSignalingText(const QString& text) {
    auto decoded = InvitationCodec::decodeText(text.trimmed());
    if (!decoded) {
        return Result<void>::failure(decoded.error());
    }
    return importSignalingDocument(InvitationCodec::encode(decoded.value()));
}

Result<Invitation> NetworkSession::decodeSignaling(const QByteArray& document) const {
    return InvitationCodec::decode(document);
}

Result<void> NetworkSession::importSignalingDocument(const QByteArray& document) {
    auto decoded = decodeSignaling(document);
    if (!decoded) {
        return Result<void>::failure(decoded.error());
    }
    const auto invitation = decoded.value();

    if (invitation.kind == Invitation::Kind::Offer) {
        if (!mesh_.meshId().isEmpty() && mesh_.meshId() != invitation.meshId) {
            return Result<void>::failure(
                "Приглашение относится к другому mesh. Сначала выйдите из текущего.");
        }
        if (connections_->contains(invitation.connectionId)) {
            return Result<void>::failure(
                "Повторно получен offer для уже известного соединения. Убедитесь, что друг "
                "отправил строку из окна «Answer готов», а не исходное приглашение.");
        }
        const auto existing = connections_->infoForPeer(invitation.fromPeer.peerId);
        if (existing && existing->open) {
            return Result<void>::failure("Этот участник уже подключён к mesh.");
        }
        connections_->discardStale(invitation.fromPeer.peerId);

        const bool joiningMesh = mesh_.meshId().isEmpty();
        if (joiningMesh) {
            clearSessionData();
            mesh_.beginJoin(app_.identity(), invitation.fromPeer, invitation.meshId);
        } else {
            mesh_.rememberPeer(invitation.fromPeer);
        }

        auto created =
            connections_->create(invitation.connectionId, invitation.fromPeer, false, false);
        if (!created) {
            if (joiningMesh) {
                mesh_.leave();
            }
            return created;
        }
        emit peerChanged(invitation.fromPeer.peerId, invitation.fromPeer.displayName, false);
        emit statusChanged("Offer импортирован. Создаётся answer…");
        const auto accepted = connections_->acceptOffer(invitation.connectionId, invitation.sdp);
        if (!accepted && joiningMesh) {
            mesh_.leave();
            clearSessionData();
        }
        return accepted;
    }

    if (mesh_.meshId() != invitation.meshId) {
        return Result<void>::failure("Answer относится к другому mesh.");
    }
    const auto connection = connections_->info(invitation.connectionId);
    if (!connection) {
        return Result<void>::failure("Не найдено исходное приглашение для этого answer.");
    }
    if (connection->answerApplied) {
        return Result<void>::failure("Этот answer уже импортирован.");
    }

    connections_->setRemote(invitation.connectionId, invitation.fromPeer);
    mesh_.rememberPeer(invitation.fromPeer);
    emit peerChanged(invitation.fromPeer.peerId, invitation.fromPeer.displayName, false);
    emit statusChanged("Answer импортирован. Устанавливается прямое P2P-соединение…");
    return connections_->acceptAnswer(invitation.connectionId, invitation.sdp);
}

void NetworkSession::emitSignaling(const QString& connectionId, const QString& type,
                                   const QString& sdp) {
    Q_UNUSED(type)
    const auto connection = connections_->info(connectionId);
    if (!connection) {
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    Invitation invitation;
    invitation.kind = connection->localOffer ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.meshId = mesh_.meshId();
    invitation.connectionId = connectionId;
    invitation.sdp = sdp;
    invitation.nonce = uuid();
    invitation.fromPeer = app_.identity();
    invitation.createdAt = now;
    invitation.expiresAt = now.addSecs(policy_.manualSignalingTimeoutSeconds);

    if (connection->meshManaged) {
        const auto packetType = connection->localOffer ? "mesh.offer" : "mesh.answer";
        broadcastService(packetType, {{"phase", connection->localOffer ? "offer" : "answer"},
                                      {"route_id", uuid()},
                                      {"hop_count", 0},
                                      {"connection_id", connectionId},
                                      {"from_peer", identityJson(app_.identity())},
                                      {"target_peer_id", connection->remote.peerId},
                                      {"sdp", sdp}});
        emit statusChanged(connection->localOffer
                               ? "Mesh offer отправлен через доступные P2P-каналы."
                               : "Mesh answer отправлен через доступные P2P-каналы.");
        return;
    }

    const auto document = InvitationCodec::encode(invitation);
    const auto text = InvitationCodec::encodeText(invitation);
    const auto kind = invitation.kind == Invitation::Kind::Offer ? "offer" : "answer";
    const auto extension = invitation.kind == Invitation::Kind::Offer ? ".tmcinvite" : ".tmcanswer";
    emit signalingReady(kind, text, document, "tiny-mesh-" + connectionId.left(8) + extension);
    emit statusChanged(invitation.kind == Invitation::Kind::Offer
                           ? "Приглашение готово. Ожидание answer."
                           : "Answer готов. Отправьте его пригласившему участнику.");
}

Packet NetworkSession::basePacket(const QString& type, const QJsonObject& payload) const {
    return {type,   uuid(), mesh_.meshId(), app_.identity().peerId, QDateTime::currentDateTimeUtc(),
            payload};
}

void NetworkSession::sendPacket(const QString& connectionId, const Packet& packet) {
    const auto bytes = PacketCodec::encode(packet);
    if (!connections_->sendText(connectionId, QString::fromUtf8(bytes))) {
        emit errorOccurred("Не удалось отправить пакет участнику.");
    }
}

void NetworkSession::sendHello(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket("peer.hello", {{"display_name", app_.identity().displayName},
                                         {"device_id", app_.identity().deviceId}}));
}

void NetworkSession::handleIncoming(const QString& connectionId, const QString& text) {
    QSet<QString> senders;
    const auto connection = connections_->info(connectionId);
    if (connection && !connection->remote.peerId.isEmpty()) {
        senders.insert(connection->remote.peerId);
    }
    auto decoded = PacketCodec::decode(text.toUtf8(), mesh_.meshId(), senders);
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "protocol", decoded.error());
        emit errorOccurred("Получен некорректный сетевой пакет: " + decoded.error());
        return;
    }
    handlePacket(connectionId, decoded.value());
}

void NetworkSession::handlePacket(const QString& connectionId, const Packet& packet) {
    if (packet.type == "peer.hello") {
        auto connection = connections_->info(connectionId);
        if (!connection) {
            return;
        }
        auto remote = connection->remote;
        if (remote.peerId.isEmpty()) {
            remote.peerId = packet.senderId;
        }
        const auto name = packet.payload.value("display_name").toString();
        if (!name.isEmpty()) {
            remote.displayName = name;
        }
        connections_->setRemote(connectionId, remote);
        const bool joined = mesh_.rememberPeer(remote);
        emit peerChanged(remote.peerId, remote.displayName, true);
        sendPacket(connectionId, basePacket("peer.hello_ack", {}));
        sendPeerList(connectionId);
        if (joined) {
            broadcastPeerList(connectionId);
        }
        ensureDynamicMesh();
        return;
    }
    if (packet.type == "peer.list") {
        handlePeerList(connectionId, packet);
        return;
    }
    if (packet.type == "chat.message") {
        const auto messageId = packet.payload.value("message_id").toString();
        auto received = messaging_.receiveMessage(
            packet, mesh_.meshId(), basePacket("chat.ack", {{"message_id", messageId}}));
        if (!received) {
            emit errorOccurred(received.error());
            return;
        }
        if (received.value().message) {
            emit messageReceived(*received.value().message, false);
        }
        sendPacket(connectionId, received.value().acknowledgement);
        return;
    }
    if (packet.type == "chat.ack") {
        if (messaging_.receiveAcknowledgement(packet)) {
            const auto messageId = packet.payload.value("message_id").toString();
            const auto counts = messaging_.deliveryCounts(messageId);
            emit deliveryChanged(messageId, counts.first, counts.second);
        }
        return;
    }
    if (packet.type == "voice.state") {
        voice_->updatePeer(packet.senderId, packet.payload.value("joined").toBool(),
                           packet.payload.value("muted").toBool());
        return;
    }
    if (packet.type == "mesh.offer") {
        handleMeshOffer(connectionId, packet);
        return;
    }
    if (packet.type == "mesh.answer") {
        handleMeshAnswer(connectionId, packet);
        return;
    }
    if (packet.type == "ping") {
        sendPacket(connectionId, basePacket("pong", packet.payload));
    }
}

void NetworkSession::sendPeerList(const QString& connectionId) {
    sendPacket(connectionId, basePacket("peer.list", {{"peers", mesh_.peerList()}}));
}

void NetworkSession::sendVoiceState(const QString& connectionId) {
    sendPacket(connectionId, basePacket("voice.state", voice_->statePayload()));
}

void NetworkSession::broadcastVoiceState() {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendVoiceState(connectionId);
    }
}

void NetworkSession::broadcastPeerList(const QString& excludedConnection) {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPeerList(connectionId);
        }
    }
}

void NetworkSession::handlePeerList(const QString& sourceConnectionId, const Packet& packet) {
    const bool changed =
        mesh_.ingestPeerList(packet.payload.value("peers").toArray(), app_.identity().peerId);
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId) {
            continue;
        }
        const auto direct = connections_->infoForPeer(peer.peerId);
        emit peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed) {
        broadcastPeerList(sourceConnectionId);
    }
    ensureDynamicMesh();
    updateMesh();
}

void NetworkSession::ensureDynamicMesh() {
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId ||
            !mesh_.shouldInitiateLink(app_.identity().peerId, peer.peerId) ||
            !mesh_.canAttemptLink(peer.peerId)) {
            continue;
        }
        const auto existing = connections_->infoForPeer(peer.peerId);
        if (existing && (existing->open || !existing->everOpened)) {
            continue;
        }
        if (existing) {
            connections_->discard(existing->connectionId);
        }
        startMeshOffer(peer);
    }
}

void NetworkSession::startMeshOffer(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || !mesh_.canAttemptLink(peer.peerId)) {
        return;
    }
    const auto existing = connections_->infoForPeer(peer.peerId);
    if (existing && (existing->open || !existing->everOpened)) {
        return;
    }
    if (existing) {
        connections_->discard(existing->connectionId);
    }

    const auto connectionId = uuid();
    auto created = connections_->create(connectionId, peer, true, true);
    if (!created) {
        mesh_.scheduleRetry(peer);
        return;
    }
    emit statusChanged("Создаётся прямой канал с " + peer.displayName + "…");
    const auto started = connections_->startOffer(connectionId);
    if (!started) {
        mesh_.scheduleRetry(peer);
    }
}

void NetworkSession::broadcastService(const QString& type, QJsonObject payload,
                                      const QString& excludedConnection) {
    mesh_.rememberRoute(payload.value("route_id").toString());
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPacket(connectionId, basePacket(type, payload));
        }
    }
}

void NetworkSession::handleMeshOffer(const QString& sourceConnectionId, const Packet& packet) {
    auto payload = packet.payload;
    const auto routeId = payload.value("route_id").toString();
    if (!mesh_.rememberRoute(routeId)) {
        return;
    }
    if (payload.value("target_peer_id").toString() != app_.identity().peerId) {
        const auto hops = payload.value("hop_count").toInt();
        if (hops < policy_.maxPeers) {
            payload["hop_count"] = hops + 1;
            broadcastService("mesh.offer", payload, sourceConnectionId);
        }
        return;
    }

    const auto connectionId = payload.value("connection_id").toString();
    const auto origin = identityFromJson(payload.value("from_peer").toObject());
    const auto existing = connections_->infoForPeer(origin.peerId);
    if (connections_->contains(connectionId) ||
        (existing && (existing->open || !existing->everOpened))) {
        return;
    }
    if (existing) {
        connections_->discard(existing->connectionId);
    }
    mesh_.rememberPeer(origin);
    auto created = connections_->create(connectionId, origin, false, true);
    if (!created) {
        return;
    }
    emit peerChanged(origin.peerId, origin.displayName, false);
    emit statusChanged("Получен автоматический offer от " + origin.displayName + "…");
    const auto accepted = connections_->acceptOffer(connectionId, payload.value("sdp").toString());
    if (!accepted) {
        emit statusChanged("Не удалось принять автоматический offer: " + accepted.error());
    }
}

void NetworkSession::handleMeshAnswer(const QString& sourceConnectionId, const Packet& packet) {
    auto payload = packet.payload;
    const auto routeId = payload.value("route_id").toString();
    if (!mesh_.rememberRoute(routeId)) {
        return;
    }
    if (payload.value("target_peer_id").toString() != app_.identity().peerId) {
        const auto hops = payload.value("hop_count").toInt();
        if (hops < policy_.maxPeers) {
            payload["hop_count"] = hops + 1;
            broadcastService("mesh.answer", payload, sourceConnectionId);
        }
        return;
    }

    const auto connectionId = payload.value("connection_id").toString();
    const auto connection = connections_->info(connectionId);
    if (!connection || !connection->meshManaged || connection->answerApplied) {
        return;
    }
    emit statusChanged("Получен mesh answer от " + connection->remote.displayName + "…");
    const auto accepted = connections_->acceptAnswer(connectionId, payload.value("sdp").toString());
    if (!accepted) {
        emit statusChanged("Не удалось принять mesh answer: " + accepted.error());
        if (connection->localOffer) {
            mesh_.scheduleRetry(connection->remote);
        }
    }
}

Result<void> NetworkSession::sendMessage(const QString& text) {
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала войдите в mesh.");
    }
    QSet<QString> targets;
    for (const auto& connection : connections_->connections()) {
        if (connection.open && !connection.remote.peerId.isEmpty()) {
            targets.insert(connection.remote.peerId);
        }
    }
    auto outgoing = messaging_.createMessage(text, mesh_.meshId(), app_.identity().peerId, targets);
    if (!outgoing) {
        return Result<void>::failure(outgoing.error());
    }
    emit messageReceived(outgoing.value().message, true);
    emit deliveryChanged(outgoing.value().message.messageId, 0,
                         outgoing.value().expectedDeliveries);
    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendPacket(connectionId, outgoing.value().packet);
    }
    return Result<void>::success();
}

Result<void> NetworkSession::startCall() {
    if (voice_->active()) {
        return Result<void>::success();
    }
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала войдите в mesh.");
    }
    if (connectedPeerCount() == 0) {
        return Result<void>::failure("Для звонка нужен хотя бы один подключённый участник.");
    }
    const auto started = voice_->start();
    if (!started) {
        return started;
    }
    emit statusChanged("Вы присоединились к голосовому звонку.");
    return Result<void>::success();
}

void NetworkSession::leaveCall() {
    if (!voice_->active()) {
        return;
    }
    voice_->leave();
    emit statusChanged("Вы вышли из голосового звонка.");
}

void NetworkSession::setMuted(bool muted) {
    if (!voice_->active() || voice_->muted() == muted) {
        return;
    }
    voice_->setMuted(muted);
    emit statusChanged(muted ? "Микрофон выключен." : "Микрофон включён.");
}

bool NetworkSession::callActive() const {
    return voice_->active();
}

bool NetworkSession::muted() const {
    return voice_->muted();
}

Result<QPair<int, int>> NetworkSession::deliveryCounts(const QString& messageId) const {
    return Result<QPair<int, int>>::success(messaging_.deliveryCounts(messageId));
}

int NetworkSession::connectedPeerCount() const {
    return connections_->connectedPeerCount();
}

int NetworkSession::knownPeerCount() const {
    return mesh_.peerCount();
}

QString NetworkSession::diagnostics() const {
    QStringList lines{"Состояние mesh: " + toString(mesh_.state()),
                      QString("Прямых каналов: %1/%2")
                          .arg(connectedPeerCount())
                          .arg(qMax(0, knownPeerCount() - 1)),
                      "TURN/relay: отключён"};
    const auto connections = connections_->connections();
    if (connections.isEmpty()) {
        lines.append("Соединения: отсутствуют");
    } else {
        lines.append("Соединения:");
    }
    for (const auto& connection : connections) {
        const auto peer = connection.remote.displayName.isEmpty() ? "не определён"
                                                                  : connection.remote.displayName;
        lines.append(QString("  %1 · transport: %2 · attempt: %3 · %4")
                         .arg(peer, toString(connection.transportState),
                              toString(connection.attemptState), connection.connectionId.left(8)));
    }
    return lines.join('\n');
}

QString NetworkSession::peerDisplayName(const QString& peerId) const {
    const auto peer = mesh_.peer(peerId);
    return peer.displayName.isEmpty() ? peerId.left(8) : peer.displayName;
}

void NetworkSession::updateMesh() {
    emit meshChanged(connectedPeerCount(), qMax(0, knownPeerCount() - 1));
}

void NetworkSession::clearSessionData() {
    messaging_.clear();
    voice_->clear();
}

} // namespace tmc
