#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/session_packet_handlers.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"
#include "tmc/protocol/packet.h"
#include "tmc/protocol/packet_codec.h"
#include "tmc/protocol/packet_dispatcher.h"
#include "tmc/signaling/invitation_codec.h"

#include <QDateTime>
#include <QJsonObject>
#include <QNetworkInformation>
#include <QSet>
#include <QUuid>

#include <chrono>
#include <utility>

namespace tmc {

namespace {

QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

NetworkSession::NetworkSession(ApplicationController& app, ConnectionPolicy policy, QObject* parent)
    : QObject(parent), app_(app), policy_(policy),
      connections_(std::make_unique<ConnectionManager>(app.config().stunServers, policy)),
      packetDispatcher_(std::make_unique<PacketDispatcher>()),
      mesh_(policy), voice_(std::make_unique<VoiceSession>(app.config().audio)) {
    Q_ASSERT(policy_.isValid());
    qRegisterMetaType<ChatMessage>();
    qRegisterMetaType<ConnectionAttemptState>();
    qRegisterMetaType<MeshSessionState>();

    SessionPacketHandlers::Callbacks handlerCallbacks;
    handlerCallbacks.localIdentity = [this] { return app_.identity(); };
    handlerCallbacks.makePacket = [this](PacketType type, PacketPayload payload) {
        return basePacket(type, std::move(payload));
    };
    handlerCallbacks.sendPacket = [this](const QString& connectionId, const Packet& packet) {
        return sendPacket(connectionId, packet);
    };
    handlerCallbacks.broadcastService =
        [this](const Packet& packet, const QString& excludedConnection) {
            broadcastService(packet, excludedConnection);
        };
    handlerCallbacks.broadcastPeerList = [this](const QString& excludedConnection) {
        broadcastPeerList(excludedConnection);
    };
    handlerCallbacks.ensureDynamicMesh = [this] { ensureDynamicMesh(); };
    handlerCallbacks.updateMesh = [this] { updateMesh(); };
    handlerCallbacks.flushRouted = [this](const QString& peerId) { flushRouted(peerId); };
    handlerCallbacks.receivePong =
        [this](const QString& connectionId, const HeartbeatPayload& payload) {
            receivePong(connectionId, payload);
        };
    handlerCallbacks.rememberAudioNegotiation =
        [this](const QString& connectionId, quint64 negotiation) {
            audioNegotiations_.insert(connectionId, negotiation);
        };
    handlerCallbacks.isCurrentAudioNegotiation =
        [this](const QString& connectionId, quint64 negotiation) {
            return audioNegotiations_.value(connectionId) == negotiation;
        };
    handlerCallbacks.peerChanged = [this](QString peerId, QString displayName, bool connected) {
        emit peerChanged(std::move(peerId), std::move(displayName), connected);
    };
    handlerCallbacks.statusChanged = [this](QString status) {
        emit statusChanged(std::move(status));
    };
    handlerCallbacks.errorOccurred = [this](QString message) {
        emit errorOccurred(std::move(message));
    };
    handlerCallbacks.messageReceived = [this](ChatMessage message, bool local) {
        emit messageReceived(std::move(message), local);
    };
    handlerCallbacks.deliveryChanged =
        [this](QString messageId, int acknowledged, int expected) {
            emit deliveryChanged(std::move(messageId), acknowledged, expected);
        };
    packetHandlers_ = std::make_unique<SessionPacketHandlers>(
        *connections_, mesh_, router_, messaging_, *voice_, policy_, std::move(handlerCallbacks));
    packetHandlers_->registerWith(*packetDispatcher_);

    connect(connections_.get(), &ConnectionManager::localDescriptionReady, this,
            &NetworkSession::emitSignaling);
    connect(connections_.get(), &ConnectionManager::audioDescriptionReady, this,
            [this](const QString& connectionId, const QString& type, const QString& sdp) {
                const auto connection = connections_->info(connectionId);
                if (!connection || !connection->open) {
                    return;
                }
                auto negotiation = audioNegotiations_.value(connectionId);
                const bool offer = type == "offer";
                if (offer) {
                    negotiation = qMax<quint64>(1, negotiation + 1);
                    audioNegotiations_.insert(connectionId, negotiation);
                }
                auto packet = basePacket(offer ? PacketType::SessionOffer
                                               : PacketType::SessionAnswer,
                                         SessionSignalingPayload{connectionId, negotiation,
                                                                 "audio", sdp});
                packet.targetId = connection->remote.peerId;
                sendPacket(connectionId, packet);
            });
    connect(connections_.get(), &ConnectionManager::statusChanged, this,
            &NetworkSession::statusChanged);
    connect(connections_.get(), &ConnectionManager::attemptChanged, this,
            [this](const QString& connectionId, ConnectionAttemptState state) {
                emit connectionAttemptChanged(connectionId, state);
                if (connectionId == manualInvitationConnectionId_) {
                    emit invitationStateChanged(true, toString(state));
                }
            });
    connect(connections_.get(), &ConnectionManager::attemptFailed, this,
            [this](const PeerIdentity& peer, bool meshManaged, bool localOffer, const QString&) {
                if (meshManaged && localOffer) {
                    mesh_.scheduleRetry(peer);
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::transportOpened, this,
            [this](const QString& connectionId, const PeerIdentity&) {
                sendHello(connectionId);
            });
    connect(connections_.get(), &ConnectionManager::linkOpened, this,
            [this](const QString& connectionId, const PeerIdentity& remote) {
                if (connectionId == manualInvitationConnectionId_) {
                    manualInvitationConnectionId_.clear();
                    emit invitationStateChanged(false, "connected");
                }
                mesh_.connectionOpened(remote);
                router_.observeDirect(remote.peerId, connectionId);
                Logger::instance().log(QtInfoMsg, "network",
                                       "Direct DataChannel opened for " + connectionId.left(8));
                emit peerChanged(remote.peerId, remote.displayName, true);
                emit statusChanged("Прямое P2P-соединение установлено. Relay не используется.");
                updateMesh();
                sendPeerList(connectionId);
                sendVoiceState(connectionId);
                broadcastPeerList(connectionId);
                announceLocalPeer();
                ensureDynamicMesh();
                sendPing(connectionId);
                if (topology_.shouldInitiateLink(app_.identity().peerId, remote.peerId)) {
                    const auto audio = connections_->startAudioOffer(connectionId);
                    if (!audio) {
                        emit errorOccurred("Не удалось начать согласование audio Track: " +
                                           audio.error());
                    }
                }
            });
    connect(connections_.get(), &ConnectionManager::linkRemoved, this,
            [this](const QString& connectionId, const PeerIdentity& remote, bool wasOpen) {
                router_.forgetConnection(connectionId);
                pendingPings_.remove(connectionId);
                audioNegotiations_.remove(connectionId);
                if (connectionId == manualInvitationConnectionId_) {
                    manualInvitationConnectionId_.clear();
                    emit invitationStateChanged(false, "finished");
                }
                const auto replacement = connections_->infoForPeer(remote.peerId);
                if (!remote.peerId.isEmpty() && (!replacement || !replacement->open)) {
                    emit peerRttChanged(remote.peerId, -1);
                    emit peerChanged(remote.peerId, remote.displayName, false);
                    voice_->removePeer(remote.peerId);
                }
                if (wasOpen) {
                    emit statusChanged("Участник отключился от прямого P2P-канала.");
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::controlTextReceived, this,
            [this](const QString& connectionId, const QString& text) {
                handleIncoming(connectionId, text, false);
            });
    connect(connections_.get(), &ConnectionManager::chatTextReceived, this,
            [this](const QString& connectionId, const QString& text) {
                handleIncoming(connectionId, text, true);
            });
    connect(connections_.get(), &ConnectionManager::audioFrameReceived, this,
            [this](const QString& connectionId, quint32 timestamp, const QByteArray& payload,
                   qint64 receivedAtNs) {
                const auto connection = connections_->info(connectionId);
                if (connection) {
                    voice_->receiveFrame(connection->remote.peerId, timestamp, payload,
                                         receivedAtNs);
                }
            });

    connect(&mesh_, &MeshCoordinator::stateChanged, this, &NetworkSession::meshStateChanged);
    connect(&mesh_, &MeshCoordinator::statusChanged, this, &NetworkSession::statusChanged);
    connect(&mesh_, &MeshCoordinator::retryRequested, this, &NetworkSession::startMeshOffer);

    connect(voice_.get(), &VoiceSession::encodedFrameReady, this,
            [this](quint32 sequence, const QByteArray& payload) {
                if (voice_->active() && !voice_->muted()) {
                    connections_->sendAudioFrameToOpen(sequence, payload);
                }
            });
    connect(voice_.get(), &VoiceSession::stateChanged, this, [this](bool active, bool muted) {
        broadcastVoiceState();
        emit callStateChanged(active, muted);
    });
    connect(voice_.get(), &VoiceSession::peerChanged, this, &NetworkSession::peerVoiceChanged);
    connect(voice_.get(), &VoiceSession::networkStatsChanged, this,
            [this](const QString& peerId, double loss, int jitter, int buffer) {
                emit peerAudioStatsChanged(peerId, loss, jitter, buffer);
                const auto connection = connections_->infoForPeer(peerId);
                if (!connection || !connection->open) {
                    return;
                }
                auto packet = basePacket(PacketType::VoiceQuality,
                                         VoiceQualityPayload{loss, jitter, buffer});
                packet.targetId = peerId;
                sendPacket(connection->connectionId, packet);
            });
    connect(voice_.get(), &VoiceSession::errorOccurred, this, &NetworkSession::errorOccurred);
    connect(voice_.get(), &VoiceSession::microphoneLevelChanged, this,
            &NetworkSession::microphoneLevelChanged);

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
            sendPing(connectionId);
            const auto connection = connections_->info(connectionId);
            if (connection &&
                (voice_->active() || connection->audioFramesAttempted > 0 ||
                 connection->audioFramesReceived > 0)) {
                Logger::instance().trace(
                    "voice_transport",
                    QString("peer=%1 track=%2 attempted=%3 sent=%4 received=%5")
                        .arg(connection->remote.peerId.left(8),
                             connection->audioTrackOpen ? "open" : "closed")
                        .arg(connection->audioFramesAttempted)
                        .arg(connection->audioFramesSent)
                        .arg(connection->audioFramesReceived));
            }
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

    if (QNetworkInformation::loadDefaultBackend()) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this] {
                    mesh_.resetRetryBackoff();
                    ensureDynamicMesh();
                });
    }
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
    if (!mesh_.meshId().isEmpty()) {
        auto leave = basePacket(PacketType::PeerLeave, PeerLeavePayload{"left"});
        leave.ttl = policy_.maxPeers;
        router_.rememberPacket(leave.packetId);
        broadcastService(leave);
    }
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
    if (!manualInvitationConnectionId_.isEmpty() &&
        connections_->contains(manualInvitationConnectionId_)) {
        return Result<void>::failure(
            "Исходящее приглашение уже создаётся или ожидает answer. Отмените его перед повтором.");
    }

    const auto connectionId = uuid();
    auto created = connections_->create(connectionId, {}, true, false);
    if (!created) {
        return created;
    }
    manualInvitationConnectionId_ = connectionId;
    emit invitationStateChanged(true, "gathering");
    emit statusChanged("Создаётся приглашение для нового участника…");
    const auto started = connections_->startOffer(connectionId);
    if (!started) {
        manualInvitationConnectionId_.clear();
        emit invitationStateChanged(false, "failed");
    }
    return started;
}

void NetworkSession::cancelInvitation() {
    if (manualInvitationConnectionId_.isEmpty()) {
        return;
    }
    const auto connectionId = manualInvitationConnectionId_;
    manualInvitationConnectionId_.clear();
    connections_->discard(connectionId);
    emit invitationStateChanged(false, "cancelled");
    emit statusChanged("Создание приглашения отменено.");
}

Result<void> NetworkSession::recreateInvitation() {
    cancelInvitation();
    return createInvitation();
}

Result<void> NetworkSession::importSignalingText(const QString& text) {
    const auto normalized = text.trimmed();
    auto decoded = normalized.startsWith('{') ? InvitationCodec::decode(normalized.toUtf8())
                                              : InvitationCodec::decodeText(normalized);
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
        const bool joiningMesh = mesh_.meshId().isEmpty();
        if (joiningMesh) {
            clearSessionData();
            mesh_.beginJoin(app_.identity(), {}, invitation.meshId);
        }

        auto created = connections_->create(invitation.connectionId, {}, false, false);
        if (!created) {
            if (joiningMesh) {
                mesh_.leave();
            }
            return created;
        }
        emit statusChanged("Offer импортирован. Создаётся answer…");
        const auto accepted = connections_->acceptOffer(invitation.connectionId, invitation.sdp);
        if (!accepted && joiningMesh) {
            mesh_.leave();
            clearSessionData();
        }
        return accepted;
    }

    if (!invitation.meshId.isEmpty() && mesh_.meshId() != invitation.meshId) {
        return Result<void>::failure("Answer относится к другому mesh.");
    }
    const auto connection = connections_->info(invitation.connectionId);
    if (!connection) {
        return Result<void>::failure("Не найдено исходное приглашение для этого answer.");
    }
    if (connection->answerApplied) {
        return Result<void>::failure("Этот answer уже импортирован.");
    }

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
    Invitation invitation;
    invitation.kind = connection->localOffer ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.meshId = mesh_.meshId();
    invitation.connectionId = connectionId;
    invitation.sdp = sdp;

    if (connection->meshManaged) {
        auto packet = basePacket(connection->localOffer ? PacketType::LinkOffer
                                                        : PacketType::LinkAnswer,
                                 LinkSignalingPayload{connectionId, connection->generation, sdp});
        packet.targetId = connection->remote.peerId;
        packet.ttl = policy_.maxPeers;
        router_.rememberPacket(packet.packetId);
        Logger::instance().log(
            QtInfoMsg, "mesh_signaling",
            QString("origin type=%1 link=%2 generation=%3 target=%4 ttl=%5")
                .arg(toString(packet.type), connectionId.left(8))
                .arg(connection->generation)
                .arg(packet.targetId.left(8))
                .arg(packet.ttl));
        // Link negotiation is rare and the mesh is capped at six peers. Flooding it over
        // every established edge is more reliable than trusting one possibly stale route;
        // packet-id deduplication and TTL keep the traffic bounded.
        broadcastService(packet);
        emit statusChanged(connection->localOffer
                               ? "Mesh offer отправлен через доступные P2P-каналы."
                               : "Mesh answer отправлен через доступные P2P-каналы.");
        return;
    }

    const auto document = InvitationCodec::encode(invitation);
    const auto text = InvitationCodec::encodeText(invitation);
    if (!text) {
        emit errorOccurred("Не удалось создать код приглашения: " + text.error());
        return;
    }
    const auto kind = invitation.kind == Invitation::Kind::Offer ? "offer" : "answer";
    emit signalingReady(kind, text.value(), document);
    emit statusChanged(invitation.kind == Invitation::Kind::Offer
                           ? "Приглашение готово. Ожидание answer."
                           : "Answer готов. Отправьте его пригласившему участнику.");
}

Packet NetworkSession::basePacket(PacketType type, PacketPayload payload) const {
    return {type,
            uuid(),
            mesh_.meshId(),
            app_.identity().peerId,
            QDateTime::currentDateTimeUtc(),
            std::move(payload)};
}

bool NetworkSession::sendPacket(const QString& connectionId, const Packet& packet) {
    const auto bytes = PacketCodec::encode(packet);
    const bool chatPacket =
        packet.type == PacketType::ChatMessage || packet.type == PacketType::ChatAck;
    const bool sent = chatPacket ? connections_->sendChat(connectionId, QString::fromUtf8(bytes))
                                 : connections_->sendControl(connectionId, QString::fromUtf8(bytes));
    if (!sent) {
        Logger::instance().log(
            QtWarningMsg, "protocol",
            QString("send_failed type=%1 connection=%2 target=%3 bytes=%4")
                .arg(toString(packet.type), connectionId.left(8), packet.targetId.left(8))
                .arg(bytes.size()));
        emit errorOccurred("Не удалось отправить пакет участнику.");
    }
    return sent;
}

void NetworkSession::sendHello(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket(PacketType::PeerHello, HelloPayload{app_.identity().displayName}));
}

void NetworkSession::sendPing(const QString& connectionId) {
    const auto nonce = uuid();
    pendingPings_.insert(connectionId, PendingPing{nonce, monotonicNs()});
    sendPacket(connectionId,
               basePacket(PacketType::Ping,
                          HeartbeatPayload{nonce, QDateTime::currentDateTimeUtc()}));
}

void NetworkSession::receivePong(const QString& connectionId,
                                 const HeartbeatPayload& payload) {
    const auto pending = pendingPings_.find(connectionId);
    if (pending == pendingPings_.end() || pending->nonce != payload.nonce) {
        return;
    }
    const auto elapsedNs = monotonicNs() - pending->sentAtNs;
    pendingPings_.erase(pending);
    const auto sampleMs = static_cast<int>((elapsedNs + 500'000) / 1'000'000);
    const auto smoothedMs = connections_->recordRoundTripTime(connectionId, sampleMs);
    const auto connection = connections_->info(connectionId);
    if (smoothedMs >= 0 && connection && !connection->remote.peerId.isEmpty()) {
        emit peerRttChanged(connection->remote.peerId, smoothedMs);
    }
}

void NetworkSession::handleIncoming(const QString& connectionId, const QString& text,
                                    bool chatChannel) {
    const auto connection = connections_->info(connectionId);
    auto decoded = PacketCodec::decode(text.toUtf8(), mesh_.meshId(), {});
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "protocol", decoded.error());
        emit errorOccurred("Получен некорректный сетевой пакет: " + decoded.error());
        return;
    }
    const bool chatPacket = decoded.value().type == PacketType::ChatMessage ||
                            decoded.value().type == PacketType::ChatAck;
    if (chatPacket != chatChannel) {
        emit errorOccurred(chatChannel ? "Служебный пакет получен в chat DataChannel."
                                       : "Пакет чата получен в control DataChannel.");
        return;
    }
    const auto type = decoded.value().type;
    const bool routed = type == PacketType::PeerAnnounce || type == PacketType::PeerLeave ||
                        type == PacketType::RouteRequest || type == PacketType::RouteReply ||
                        type == PacketType::LinkOffer || type == PacketType::LinkAnswer;
    if (!routed && connection && !connection->remote.peerId.isEmpty() &&
        decoded.value().senderId != connection->remote.peerId) {
        emit errorOccurred("Packet sender does not match the direct WebRTC peer.");
        return;
    }
    if (connection && !connection->open && decoded.value().type != PacketType::PeerHello) {
        emit errorOccurred("До завершения handshake разрешён только peer.hello.");
        return;
    }
    if (!packetDispatcher_->dispatch({connectionId}, decoded.value())) {
        emit errorOccurred("Для типа пакета не зарегистрирован обработчик: " +
                           toString(decoded.value().type));
    }
}

void NetworkSession::sendPeerList(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket(PacketType::PeerSnapshot,
                          PeerSnapshotPayload{mesh_.revision(), mesh_.peerList()}));
}

void NetworkSession::announceLocalPeer(const QString& excludedConnection) {
    auto packet = basePacket(PacketType::PeerAnnounce,
                             PeerAnnouncePayload{app_.identity(), mesh_.revision(), 0});
    packet.ttl = policy_.maxPeers;
    router_.rememberPacket(packet.packetId);
    broadcastService(packet, excludedConnection);
}

void NetworkSession::sendVoiceState(const QString& connectionId) {
    sendPacket(connectionId, basePacket(PacketType::VoiceState,
                                        VoiceStatePayload{voice_->active(), voice_->muted()}));
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

void NetworkSession::ensureDynamicMesh() {
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId ||
            !topology_.shouldInitiateLink(app_.identity().peerId, peer.peerId) ||
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
    const auto generation = mesh_.nextLinkGeneration(peer.peerId);
    auto created = connections_->create(connectionId, peer, true, true, generation);
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

void NetworkSession::broadcastService(const Packet& packet, const QString& excludedConnection) {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPacket(connectionId, packet);
        }
    }
}

bool NetworkSession::sendRouted(Packet packet, const QString& excludedConnection) {
    if (packet.targetId.isEmpty()) {
        return false;
    }
    const auto direct = connections_->infoForPeer(packet.targetId);
    if (direct && direct->open && direct->connectionId != excludedConnection) {
        return sendPacket(direct->connectionId, packet);
    }
    const auto nextHop = router_.nextHop(packet.targetId, excludedConnection);
    if (nextHop && connections_->info(*nextHop) && connections_->info(*nextHop)->open) {
        return sendPacket(*nextHop, packet);
    }
    if (packet.senderId == app_.identity().peerId) {
        auto& queue = pendingRouted_[packet.targetId];
        if (packet.type == PacketType::LinkOffer || packet.type == PacketType::LinkAnswer) {
            queue.clear();
        }
        if (queue.size() < 16) {
            queue.append(std::move(packet));
        }
        requestRoute(queue.constFirst().targetId);
    }
    return false;
}

void NetworkSession::requestRoute(const QString& peerId) {
    if (peerId.isEmpty() || routeRequests_.contains(peerId)) {
        return;
    }
    const auto requestId = uuid();
    routeRequests_.insert(peerId, requestId);
    auto request = basePacket(PacketType::RouteRequest, RoutePayload{requestId, 0});
    request.targetId = peerId;
    request.ttl = policy_.maxPeers;
    router_.rememberPacket(request.packetId);
    broadcastService(request);
    QTimer::singleShot(5000, this, [this, peerId, requestId] {
        if (routeRequests_.value(peerId) == requestId) {
            routeRequests_.remove(peerId);
        }
    });
}

void NetworkSession::flushRouted(const QString& peerId) {
    routeRequests_.remove(peerId);
    const auto packets = pendingRouted_.take(peerId);
    for (auto packet : packets) {
        sendRouted(std::move(packet));
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

void NetworkSession::setDeafened(bool deafened) {
    voice_->setDeafened(deafened);
    emit audioStateChanged();
}

void NetworkSession::setMicrophoneTest(bool enabled) {
    voice_->setMicrophoneTest(enabled);
    emit audioStateChanged();
}

void NetworkSession::setPeerVolume(const QString& peerId, int percent) {
    voice_->setPeerVolume(peerId, percent);
}

Result<void> NetworkSession::applyAudioPreferences(const AudioPreferences& preferences) {
    const auto result = voice_->applyPreferences(preferences);
    if (result) {
        emit audioStateChanged();
    }
    return result;
}

QPair<QStringList, QStringList> NetworkSession::refreshAudioDevices() {
    return voice_->refreshDevices();
}

bool NetworkSession::callActive() const {
    return voice_->active();
}

bool NetworkSession::muted() const {
    return voice_->muted();
}

bool NetworkSession::deafened() const {
    return voice_->deafened();
}

bool NetworkSession::microphoneTest() const {
    return voice_->microphoneTest();
}

bool NetworkSession::invitationPending() const {
    return !manualInvitationConnectionId_.isEmpty() &&
           connections_->contains(manualInvitationConnectionId_);
}

QString NetworkSession::invitationState() const {
    const auto connection = connections_->info(manualInvitationConnectionId_);
    return connection ? toString(connection->attemptState) : QString{};
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
        const auto elapsed =
            qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - connection.createdAtMs) / 1000;
        lines.append(QString("  %1 · transport: %2 · ICE: %3 · attempt: %4 · %5 s · %6")
                         .arg(peer, toString(connection.transportState), connection.iceState,
                              toString(connection.attemptState))
                         .arg(elapsed)
                         .arg(connection.connectionId.left(8)));
        lines.append(QString("    candidates: host=%1 srflx=%2 relay=%3 · selected: %4")
                         .arg(connection.hostCandidates)
                         .arg(connection.serverReflexiveCandidates)
                         .arg(connection.relayCandidates)
                         .arg(connection.selectedCandidatePair.isEmpty()
                                  ? "не выбрана"
                                  : connection.selectedCandidatePair));
        lines.append(QString("    channels: control=%1 chat=%2 audio-rtp=%3 hello=%4 · "
                             "buffered: control=%5 chat=%6 · queued: control=%7 chat=%8")
                         .arg(connection.controlChannelOpen ? "open" : "closed",
                              connection.chatChannelOpen ? "open" : "closed",
                              connection.audioTrackOpen ? "open" : "closed",
                              connection.helloReceived ? "yes" : "no")
                         .arg(connection.controlBufferedBytes)
                         .arg(connection.chatBufferedBytes)
                         .arg(connection.queuedControlBytes)
                         .arg(connection.queuedChatBytes));
        lines.append(QString("    audio frames: attempted=%1 sent=%2 received=%3")
                         .arg(connection.audioFramesAttempted)
                         .arg(connection.audioFramesSent)
                         .arg(connection.audioFramesReceived));
    }
    const auto recent = connections_->recentAttempts();
    if (!recent.isEmpty()) {
        lines.append("Последние завершённые попытки:");
        for (const auto& connection : recent) {
            lines.append(QString("  %1 · %2 · ICE: %3 · host=%4 srflx=%5 relay=%6%7")
                             .arg(connection.connectionId.left(8),
                                  toString(connection.attemptState), connection.iceState)
                             .arg(connection.hostCandidates)
                             .arg(connection.serverReflexiveCandidates)
                             .arg(connection.relayCandidates)
                             .arg(connection.lastError.isEmpty() ? QString{}
                                                                 : " · " + connection.lastError));
        }
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
    router_.clear();
    pendingRouted_.clear();
    routeRequests_.clear();
    audioNegotiations_.clear();
    pendingPings_.clear();
}

} // namespace tmc
