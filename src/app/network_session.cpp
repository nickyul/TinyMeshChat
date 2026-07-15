#include "app/network_session.h"

#include "app/application_controller.h"
#include "core/logger.h"
#include "network/peer_connection.h"
#include "protocol/packet.h"
#include "protocol/packet_codec.h"
#include "signaling/invitation_codec.h"
#include "storage/message_repository.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
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

static QJsonObject messageJson(const ChatMessage& message) {
    return {{"message_id", message.messageId},
            {"sender_id", message.senderId},
            {"logical_clock", message.logicalClock},
            {"created_at", message.createdAt.toUTC().toString(Qt::ISODateWithMs)},
            {"text", message.text}};
}

NetworkSession::NetworkSession(ApplicationController& app, QObject* parent)
    : QObject(parent), app_(app), messages_(std::make_unique<MessageRepository>(app.database())) {
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

Result<bool> NetworkSession::restoreLastRoom() {
    if (!roomId_.isEmpty())
        return Result<bool>::success(false);
    QSqlQuery roomQuery(app_.database());
    if (!roomQuery.exec("SELECT room_id,room_name,created_by FROM rooms "
                        "ORDER BY created_at DESC LIMIT 1"))
        return Result<bool>::failure(roomQuery.lastError().text());
    if (!roomQuery.next())
        return Result<bool>::success(false);

    roomId_ = roomQuery.value(0).toString();
    roomName_ = roomQuery.value(1).toString();

    QSqlQuery clockQuery(app_.database());
    clockQuery.prepare("SELECT COALESCE(MAX(logical_clock),0) FROM messages WHERE room_id=?");
    clockQuery.addBindValue(roomId_);
    if (clockQuery.exec() && clockQuery.next())
        logicalClock_ = clockQuery.value(0).toLongLong();

    emit roomChanged(roomId_, roomName_);

    QSqlQuery peersQuery(app_.database());
    peersQuery.prepare("SELECT peer_id,display_name FROM peers WHERE room_id=? AND peer_id<>?");
    peersQuery.addBindValue(roomId_);
    peersQuery.addBindValue(app_.identity().peerId);
    if (peersQuery.exec()) {
        while (peersQuery.next())
            emit peerChanged(peersQuery.value(0).toString(), peersQuery.value(1).toString(), false);
    }

    emit statusChanged("Последняя комната восстановлена. Создайте новые приглашения для связи.");
    return Result<bool>::success(true);
}

Result<void> NetworkSession::createRoom(const QString& name) {
    const auto normalized = name.trimmed();
    if (normalized.isEmpty())
        return Result<void>::failure("Введите название комнаты.");
    if (!links_.isEmpty())
        return Result<void>::failure(
            "Сначала завершите текущие соединения перезапуском приложения.");

    roomId_ = uuid();
    roomName_ = normalized;
    auto result = ensureRoom(roomId_, roomName_, app_.identity().peerId);
    if (!result) {
        roomId_.clear();
        roomName_.clear();
        return result;
    }
    persistPeer(app_.identity());

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

        roomId_ = invitation.roomId;
        roomName_ = invitation.roomName;
        auto room = ensureRoom(roomId_, roomName_, invitation.fromPeer.peerId);
        if (!room)
            return room;

        auto link = makeLink(invitation.connectionId);
        link->remote = invitation.fromPeer;
        persistPeer(link->remote);
        persistPeer(app_.identity());
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
    persistPeer(link->remote);
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

Result<void> NetworkSession::ensureRoom(const QString& id, const QString& name,
                                        const QString& creator) {
    QSqlQuery query(app_.database());
    query.prepare("INSERT OR IGNORE INTO rooms(room_id,room_name,created_by,created_at) "
                  "VALUES(?,?,?,?)");
    query.addBindValue(id);
    query.addBindValue(name);
    query.addBindValue(creator);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec())
        return Result<void>::failure("Ошибка SQLite: " + query.lastError().text());
    return Result<void>::success();
}

bool NetworkSession::persistPeer(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || roomId_.isEmpty())
        return false;
    QSqlQuery existing(app_.database());
    existing.prepare("SELECT 1 FROM peers WHERE room_id=? AND peer_id=?");
    existing.addBindValue(roomId_);
    existing.addBindValue(peer.peerId);
    const bool known = existing.exec() && existing.next();
    QSqlQuery query(app_.database());
    query.prepare("INSERT INTO peers(peer_id,room_id,display_name,device_id,added_at) "
                  "VALUES(?,?,?,?,?) ON CONFLICT(room_id,peer_id) DO UPDATE SET "
                  "display_name=excluded.display_name,device_id=excluded.device_id");
    query.addBindValue(peer.peerId);
    query.addBindValue(roomId_);
    query.addBindValue(peer.displayName.isEmpty() ? peer.peerId.left(8) : peer.displayName);
    query.addBindValue(peer.deviceId.isEmpty() ? "unknown" : peer.deviceId);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        Logger::instance().log(QtWarningMsg, "storage",
                               "Could not persist peer: " + query.lastError().text());
        return false;
    }
    return !known;
}

QList<PeerIdentity> NetworkSession::knownPeers() const {
    QList<PeerIdentity> peers;
    QSqlQuery query(app_.database());
    query.prepare("SELECT peer_id,display_name,device_id,added_at FROM peers WHERE room_id=?");
    query.addBindValue(roomId_);
    if (!query.exec())
        return peers;
    while (query.next())
        peers.append({query.value(0).toString(), query.value(1).toString(),
                      query.value(2).toString(),
                      QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs)});
    return peers;
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
        sendSyncSummary(link);
        resendPending(link);
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

void NetworkSession::resendPending(const std::shared_ptr<Link>& link) {
    if (link->remote.peerId.isEmpty())
        return;
    const auto pending = messages_->pendingForPeer(roomId_, link->remote.peerId);
    if (!pending) {
        emit errorOccurred("Не удалось загрузить неподтверждённые сообщения: " + pending.error());
        return;
    }
    if (pending.value().isEmpty())
        return;

    for (const auto& message : pending.value()) {
        delivery_.track(message.messageId, {link->remote.peerId});
        messages_->setDelivery(message.messageId, link->remote.peerId, "pending");
        sendPacket(link, basePacket("chat.message", {{"message_id", message.messageId},
                                                     {"text", message.text},
                                                     {"logical_clock", message.logicalClock}}));
    }
    emit statusChanged(
        QString("Повторно отправлено неподтверждённых сообщений: %1").arg(pending.value().size()));
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
        const bool joined = persistPeer(link->remote);
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
        auto inserted = messages_->insert(message);
        if (!inserted) {
            emit errorOccurred(inserted.error());
            return;
        }
        if (inserted.value())
            emit messageReceived(message, false);
        sendPacket(link, basePacket("chat.ack", {{"message_id", message.messageId}}));
        return;
    }

    if (packet.type == "chat.ack") {
        const auto messageId = packet.payload.value("message_id").toString();
        messages_->setDelivery(messageId, packet.senderId, "delivered");
        delivery_.acknowledge(messageId, packet.senderId);
        const auto counts = messages_->deliveryCounts(messageId);
        if (counts)
            emit deliveryChanged(messageId, counts.value().first, counts.value().second);
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

    if (packet.type == "sync.summary") {
        handleSyncSummary(link, packet);
        return;
    }

    if (packet.type == "sync.request") {
        handleSyncRequest(link, packet);
        return;
    }

    if (packet.type == "sync.messages") {
        handleSyncMessages(link, packet);
        return;
    }

    if (packet.type == "ping")
        sendPacket(link, basePacket("pong", packet.payload));
}

void NetworkSession::sendSyncSummary(const std::shared_ptr<Link>& link) {
    const auto recent = messages_->recent(roomId_, 500);
    const auto total = messages_->count(roomId_);
    if (!recent || !total) {
        emit errorOccurred("Не удалось подготовить сводку истории для синхронизации.");
        return;
    }
    QJsonArray ids;
    for (const auto& message : recent.value())
        ids.append(message.messageId);
    sendPacket(link,
               basePacket("sync.summary", {{"message_count", total.value()}, {"recent_ids", ids}}));
}

void NetworkSession::handleSyncSummary(const std::shared_ptr<Link>& link, const Packet& packet) {
    const auto remoteIds = packet.payload.value("recent_ids").toArray();
    if (remoteIds.size() > 500) {
        emit errorOccurred("Сводка истории содержит слишком много идентификаторов.");
        return;
    }
    const auto local = messages_->recent(roomId_, 500);
    if (!local) {
        emit errorOccurred(local.error());
        return;
    }
    QSet<QString> localIds;
    for (const auto& message : local.value())
        localIds.insert(message.messageId);

    QStringList missing;
    for (const auto& value : remoteIds) {
        const auto id = value.toString();
        if (QUuid::fromString(id).isNull()) {
            emit errorOccurred("Сводка истории содержит некорректный UUID.");
            return;
        }
        if (!localIds.contains(id))
            missing.append(id);
    }
    for (qsizetype offset = 0; offset < missing.size(); offset += 200) {
        QJsonArray requested;
        const auto end = qMin(offset + 200, missing.size());
        for (qsizetype i = offset; i < end; ++i)
            requested.append(missing[i]);
        sendPacket(link, basePacket("sync.request", {{"message_ids", requested}}));
    }
    if (missing.isEmpty())
        emit statusChanged("История с участником синхронизирована.");
    else
        emit statusChanged(QString("Запрошено пропущенных сообщений: %1").arg(missing.size()));
}

void NetworkSession::handleSyncRequest(const std::shared_ptr<Link>& link, const Packet& packet) {
    const auto requested = packet.payload.value("message_ids").toArray();
    if (requested.isEmpty() || requested.size() > 200) {
        emit errorOccurred("Некорректный запрос синхронизации истории.");
        return;
    }
    QStringList ids;
    for (const auto& value : requested) {
        const auto id = value.toString();
        if (QUuid::fromString(id).isNull()) {
            emit errorOccurred("Запрос истории содержит некорректный UUID.");
            return;
        }
        ids.append(id);
    }
    const auto found = messages_->byIds(roomId_, ids);
    if (!found) {
        emit errorOccurred(found.error());
        return;
    }
    sendSyncMessages(link, found.value());
}

void NetworkSession::sendSyncMessages(const std::shared_ptr<Link>& link,
                                      const QList<ChatMessage>& messages) {
    QJsonArray batch;
    for (const auto& message : messages) {
        batch.append(messageJson(message));
        auto candidate = basePacket("sync.messages", {{"messages", batch}});
        if (PacketCodec::encode(candidate).size() < PacketCodec::MaxBytes - 1024)
            continue;
        batch.removeLast();
        if (!batch.isEmpty())
            sendPacket(link, basePacket("sync.messages", {{"messages", batch}}));
        batch = QJsonArray{messageJson(message)};
    }
    if (!batch.isEmpty())
        sendPacket(link, basePacket("sync.messages", {{"messages", batch}}));
}

void NetworkSession::handleSyncMessages(const std::shared_ptr<Link>&, const Packet& packet) {
    const auto values = packet.payload.value("messages").toArray();
    if (values.isEmpty() || values.size() > 200) {
        emit errorOccurred("Получен некорректный пакет истории.");
        return;
    }
    int insertedCount = 0;
    for (const auto& value : values) {
        const auto object = value.toObject();
        const auto now = QDateTime::currentDateTimeUtc();
        ChatMessage message{
            object.value("message_id").toString(),
            roomId_,
            object.value("sender_id").toString(),
            object.value("text").toString(),
            object.value("logical_clock").toInteger(),
            QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs),
            now};
        if (!message.isValid()) {
            emit errorOccurred("Пакет истории содержит некорректное сообщение.");
            return;
        }
        const auto inserted = messages_->insert(message);
        if (!inserted) {
            emit errorOccurred(inserted.error());
            return;
        }
        logicalClock_ = qMax(logicalClock_, message.logicalClock) + 1;
        if (inserted.value()) {
            ++insertedCount;
            emit messageReceived(message, message.senderId == app_.identity().peerId);
        }
    }
    emit statusChanged(
        QString("Синхронизация истории: получено новых сообщений %1").arg(insertedCount));
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
        if (persistPeer(peer)) {
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
    persistPeer(origin);
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
    auto inserted = messages_->insert(message);
    if (!inserted)
        return Result<void>::failure(inserted.error());

    QSet<QString> targets;
    QSqlQuery peerQuery(app_.database());
    peerQuery.prepare("SELECT peer_id FROM peers WHERE room_id=? AND peer_id<>?");
    peerQuery.addBindValue(roomId_);
    peerQuery.addBindValue(app_.identity().peerId);
    if (!peerQuery.exec())
        return Result<void>::failure(peerQuery.lastError().text());
    while (peerQuery.next())
        targets.insert(peerQuery.value(0).toString());
    for (const auto& link : links_) {
        if (link->remote.peerId.isEmpty())
            continue;
        targets.insert(link->remote.peerId);
    }
    for (const auto& target : targets)
        messages_->setDelivery(message.messageId, target, "pending");
    delivery_.track(message.messageId, targets);
    emit messageReceived(message, true);
    emit deliveryChanged(message.messageId, 0, targets.size());

    const auto packet = basePacket("chat.message", {{"message_id", message.messageId},
                                                    {"text", message.text},
                                                    {"logical_clock", message.logicalClock}});
    for (const auto& link : links_)
        sendPacket(link, packet);
    return Result<void>::success();
}

Result<QList<ChatMessage>> NetworkSession::history() const {
    if (roomId_.isEmpty())
        return Result<QList<ChatMessage>>::success({});
    return messages_->history(roomId_);
}

Result<QPair<int, int>> NetworkSession::deliveryCounts(const QString& messageId) const {
    return messages_->deliveryCounts(messageId);
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
    QSqlQuery query(app_.database());
    query.prepare("SELECT display_name FROM peers WHERE room_id=? AND peer_id=?");
    query.addBindValue(roomId_);
    query.addBindValue(peerId);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return peerId.left(8);
}

void NetworkSession::updateMesh() {
    emit meshChanged(connectedPeerCount(), qMax(0, knownPeerCount() - 1));
}
