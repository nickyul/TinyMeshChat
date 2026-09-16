#include "tmc/rendezvous/rendezvous_service.h"

#include "tmc/core/logger.h"
#include "tmc/signaling/invitation_codec.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace tmc {

namespace {

constexpr quint32 WireMagic = 0x544D5256; // TMRV
constexpr quint8 WireVersion = 1;
constexpr qsizetype AuthenticationTagBytes = 32;
constexpr qsizetype WireHeaderBytes = 86;
constexpr qsizetype MaxDatagramBytes = 1200;
constexpr qsizetype MaxChunkPayloadBytes =
    MaxDatagramBytes - WireHeaderBytes - AuthenticationTagBytes;
constexpr int TimestampWindowMs = 120000;
constexpr int ReplayRetentionMs = 300000;
constexpr int MaxReplayEntriesPerContact = 256;
constexpr int SignalTimeoutMs = 20000;
constexpr int ConnectTimeoutMs = 60000;
constexpr int MaxIncomingSignals = 8;
constexpr int MaxIncomingRequests = 16;
constexpr int MaxOutgoingSignals = 8;
constexpr int MaxSignalChunks = 320;
constexpr quint16 MinDynamicPort = 49152;
constexpr quint32 StunMagicCookie = 0x2112A442;

constexpr std::array<qint64, 5> ProbeBurstMs{0, 250, 1000, 3000, 8000};
constexpr std::array<qint64, 4> OfflineRetryMs{30000, 60000, 120000, 300000};
constexpr std::array<qint64, 6> ConnectRetryMs{0, 500, 1500, 3500, 7000, 10000};
constexpr std::array<qint64, 5> SignalRetryMs{0, 250, 750, 1500, 3000};

QString normalizedPeerId(const QString& value) {
    const QUuid id(value);
    return id.isNull() ? QString{} : id.toString(QUuid::WithoutBraces).toLower();
}

bool constantTimeEqual(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) {
        return false;
    }
    uchar difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<uchar>(left.at(i)) ^ static_cast<uchar>(right.at(i));
    }
    return difference == 0;
}

qint64 withJitter(qint64 interval) {
    if (interval <= 0) {
        return 0;
    }
    const auto spread = qMax<qint64>(1, interval / 5);
    const auto random = QRandomGenerator::global()->bounded(static_cast<quint32>(spread));
    return interval - spread / 2 + random;
}

QString localIpv4Address() {
    QString fallback;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) ||
            !(interface.flags() & QNetworkInterface::IsRunning) ||
            (interface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                !entry.ip().isLoopback() && !entry.ip().isLinkLocal()) {
                const auto address = entry.ip().toString();
                if (fallback.isEmpty()) {
                    fallback = address;
                }
                if ((interface.type() == QNetworkInterface::Ethernet ||
                     interface.type() == QNetworkInterface::Wifi) &&
                    entry.ip().isPrivateUse()) {
                    return address;
                }
            }
        }
    }
    return fallback.isEmpty() ? QStringLiteral("127.0.0.1") : fallback;
}

bool isPublicIpv4(const QHostAddress& address) {
    static const QHostAddress sharedAddressSpace("100.64.0.0");
    return address.protocol() == QAbstractSocket::IPv4Protocol && address.isGlobal() &&
           !address.isInSubnet(sharedAddressSpace, 10);
}

void appendU16(QByteArray& bytes, quint16 value) {
    bytes.append(static_cast<char>((value >> 8) & 0xff));
    bytes.append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray& bytes, quint32 value) {
    bytes.append(static_cast<char>((value >> 24) & 0xff));
    bytes.append(static_cast<char>((value >> 16) & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
    bytes.append(static_cast<char>(value & 0xff));
}

quint16 readU16(const QByteArray& bytes, qsizetype offset) {
    return (static_cast<quint16>(static_cast<uchar>(bytes.at(offset))) << 8) |
           static_cast<uchar>(bytes.at(offset + 1));
}

quint32 readU32(const QByteArray& bytes, qsizetype offset) {
    return (static_cast<quint32>(static_cast<uchar>(bytes.at(offset))) << 24) |
           (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 1))) << 16) |
           (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 2))) << 8) |
           static_cast<uchar>(bytes.at(offset + 3));
}

QString signalKey(const QString& peerId, const QByteArray& signalId) {
    return peerId + ':' + QString::fromLatin1(signalId.toHex());
}

} // namespace

QString contactStatusName(ContactStatus status) {
    switch (status) {
    case ContactStatus::Probing:
        return "Проверка";
    case ContactStatus::Online:
        return "В сети";
    case ContactStatus::Offline:
        return "Не в сети";
    case ContactStatus::Busy:
        return "Занят";
    case ContactStatus::Connected:
        return "Подключён";
    }
    return "Не в сети";
}

RendezvousService::RendezvousService(PeerIdentity localIdentity,
                                     RendezvousPreferences preferences,
                                     QStringList stunServers, QString contactsPath,
                                     std::unique_ptr<IPortMapper> portMapper, QObject* parent)
    : QObject(parent), localIdentity_(std::move(localIdentity)),
      preferences_(std::move(preferences)), stunServers_(std::move(stunServers)),
      store_(std::move(contactsPath)),
      portMapper_(portMapper ? std::move(portMapper)
                             : std::make_unique<LibPlumPortMapper>()) {
    if (!preferences_.lastPublicAddress.isEmpty() && preferences_.lastPublicPort != 0) {
        const QHostAddress cachedAddress(preferences_.lastPublicAddress);
        if (isPublicIpv4(cachedAddress)) {
            publicEndpoint_ = {cachedAddress.toString(), preferences_.lastPublicPort};
            mappingMethod_ = preferences_.mappingMethod;
        } else {
            Logger::instance().log(
                QtInfoMsg, "rendezvous",
                "Ignoring cached non-public rendezvous endpoint " +
                    preferences_.lastPublicAddress + ':' +
                    QString::number(preferences_.lastPublicPort));
        }
    }
    qRegisterMetaType<ContactPresence>();
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &RendezvousService::tick);
    connect(&socket_, &QUdpSocket::readyRead, this, [this] {
        while (socket_.hasPendingDatagrams()) {
            const auto datagram = socket_.receiveDatagram(MaxDatagramBytes + 1);
            if (datagram.isValid()) {
                processDatagram(datagram.data(), datagram.senderAddress(), datagram.senderPort());
            }
        }
    });
    connect(portMapper_.get(), &IPortMapper::mappingReady, this,
            [this](const PortMappingResult& result) {
                QHostAddress address;
                if (!address.setAddress(result.publicAddress) || result.publicPort == 0) {
                    Logger::instance().log(QtWarningMsg, "rendezvous",
                                           "Port mapper returned an invalid IPv4 endpoint");
                    return;
                }
                if (!isPublicIpv4(address)) {
                    explicitMappingActive_ = false;
                    Logger::instance().log(
                        QtInfoMsg, "rendezvous",
                        QString("Ignoring non-public %1 mapping endpoint %2:%3; "
                                "waiting for STUN")
                            .arg(portMappingMethodName(result.method), address.toString())
                            .arg(result.publicPort));
                    if (stunEndpoint_.port != 0) {
                        publicEndpoint_ = stunEndpoint_;
                        mappingMethod_ = "STUN";
                        emit externalEndpointChanged(publicEndpoint_.address,
                                                     publicEndpoint_.port, mappingMethod_);
                    } else {
                        publicEndpoint_ = {};
                        mappingMethod_ = "unavailable";
                    }
                    return;
                }
                explicitMappingActive_ = true;
                publicEndpoint_ = {address.toString(), result.publicPort};
                mappingMethod_ = portMappingMethodName(result.method);
                emit externalEndpointChanged(publicEndpoint_.address, publicEndpoint_.port,
                                             mappingMethod_);
            });
    connect(portMapper_.get(), &IPortMapper::mappingFailed, this,
            [this](const QString& reason) {
                explicitMappingActive_ = false;
                Logger::instance().log(QtInfoMsg, "rendezvous",
                                       "Port mapping unavailable: " + reason);
                if (stunEndpoint_.port != 0) {
                    publicEndpoint_ = stunEndpoint_;
                    mappingMethod_ = "STUN";
                    emit externalEndpointChanged(publicEndpoint_.address,
                                                 publicEndpoint_.port, mappingMethod_);
                } else {
                    mappingMethod_ = "unavailable";
                }
            });
}

RendezvousService::~RendezvousService() {
    stop();
}

Result<void> RendezvousService::start() {
    if (running_) {
        return Result<void>::success();
    }
    const auto loaded = store_.load();
    if (!loaded) {
        return Result<void>::failure(loaded.error());
    }
    contacts_.clear();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& record : loaded.value()) {
        if (record.identity.peerId == localIdentity_.peerId) {
            continue;
        }
        ContactRuntime runtime;
        runtime.record = record;
        runtime.status = ContactStatus::Probing;
        runtime.nextProbeMs = now + QRandomGenerator::global()->bounded(151u);
        runtime.lastPersistedMs = now;
        contacts_.insert(record.identity.peerId, std::move(runtime));
    }
    if (!bindSocket()) {
        return Result<void>::failure("Cannot bind a rendezvous UDP port");
    }
    running_ = true;
    timer_.start();
    startEndpointDiscovery();
    for (const auto& contact : contacts()) {
        emit contactChanged(contact);
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               "Probe scheduled for " + contact.contact.identity.displayName);
    }
    return Result<void>::success();
}

void RendezvousService::stop() {
    if (!running_ && socket_.state() != QAbstractSocket::BoundState) {
        return;
    }
    running_ = false;
    timer_.stop();
    portMapper_->stop();
    socket_.close();
    outgoingRequests_.clear();
    incomingRequests_.clear();
    outgoingSignals_.clear();
    incomingSignals_.clear();
    Logger::instance().log(QtInfoMsg, "rendezvous", "Rendezvous service stopped");
}

bool RendezvousService::bindSocket() {
    for (const auto port : preferences_.localPorts) {
        if (socket_.bind(QHostAddress::AnyIPv4, port)) {
            localEndpoint_ = {localIpv4Address(), port};
            preferences_.lastBoundPort = port;
            emit persistentPortsChanged(preferences_.localPorts, port);
            Logger::instance().log(QtInfoMsg, "rendezvous",
                                   QString("Rendezvous socket bound to UDP %1").arg(port));
            return true;
        }
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto port = static_cast<quint16>(QRandomGenerator::system()->bounded(
            static_cast<quint32>(MinDynamicPort), static_cast<quint32>(65536)));
        if (preferences_.localPorts.contains(port) ||
            !socket_.bind(QHostAddress::AnyIPv4, port)) {
            continue;
        }
        const auto replaced = preferences_.localPorts.last();
        preferences_.localPorts.last() = port;
        preferences_.lastBoundPort = port;
        localEndpoint_ = {localIpv4Address(), port};
        emit persistentPortsChanged(preferences_.localPorts, port);
        Logger::instance().log(
            QtWarningMsg, "rendezvous",
            QString("All persistent ports were busy; replaced fallback %1 with %2")
                .arg(replaced)
                .arg(port));
        return true;
    }
    Logger::instance().log(QtWarningMsg, "rendezvous",
                           "Failed to bind all persistent and replacement UDP ports");
    return false;
}

void RendezvousService::startEndpointDiscovery() {
    if (socket_.state() != QAbstractSocket::BoundState) {
        Logger::instance().log(
            QtWarningMsg, "rendezvous",
            QString("Endpoint discovery skipped: UDP socket state is %1 (open=%2)")
                .arg(static_cast<int>(socket_.state()))
                .arg(socket_.isOpen()));
        return;
    }
    // STUN is deliberately kicked off first. Router mapping discovery is asynchronous in
    // libplum, but it must never delay the direct-endpoint fallback on a router without
    // PCP/NAT-PMP/UPnP support.
    Logger::instance().log(
        QtInfoMsg, "rendezvous",
        QString("Endpoint discovery started for local UDP %1; preferred external %2")
            .arg(localEndpoint_.port)
            .arg(preferences_.lastPublicPort));
    startStunDiscovery();
    portMapper_->start(localEndpoint_.port, preferences_.lastPublicPort);
}

void RendezvousService::restartDiscovery() {
    if (!running_) {
        return;
    }
    localEndpoint_.address = localIpv4Address();
    stunEndpoint_ = {};
    stunAttempts_ = 0;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (auto& contact : contacts_) {
        contact.status = ContactStatus::Probing;
        contact.probeStep = 0;
        contact.probeAttemptId.clear();
        contact.nextProbeMs = now + QRandomGenerator::global()->bounded(151u);
        emit contactChanged({contact.record, contact.status, contact.requestPending});
    }
    startEndpointDiscovery();
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           "Network change detected; endpoint discovery restarted");
}

void RendezvousService::startStunDiscovery() {
    if (stunServers_.isEmpty()) {
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               "STUN discovery skipped: no STUN servers configured");
        return;
    }
    const auto uri = stunServers_.at(stunServerIndex_ % stunServers_.size()).mid(5);
    auto host = uri;
    quint16 port = 3478;
    const auto colon = uri.lastIndexOf(':');
    if (colon > 0 && !uri.contains(']')) {
        bool validPort = false;
        const auto parsedPort = uri.mid(colon + 1).toUShort(&validPort);
        if (validPort) {
            host = uri.left(colon);
            port = parsedPort;
        }
    }
    QHostInfo::lookupHost(host, this, [this, host, port](const QHostInfo& info) {
        if (!running_) {
            return;
        }
        const auto addresses = info.addresses();
        const auto address = std::find_if(addresses.cbegin(), addresses.cend(),
                                          [](const QHostAddress& candidate) {
                                              return candidate.protocol() ==
                                                     QAbstractSocket::IPv4Protocol;
                                          });
        if (info.error() != QHostInfo::NoError || address == addresses.cend()) {
            Logger::instance().log(QtInfoMsg, "rendezvous",
                                   "STUN lookup failed for " + host);
            ++stunAttempts_;
            ++stunServerIndex_;
            QTimer::singleShot(stunAttempts_ <= qMax(2, stunServers_.size() * 2) ? 1500
                                                                                : 60000,
                               this, [this] {
                                   if (running_ && stunEndpoint_.port == 0) {
                                       startStunDiscovery();
                                   }
                               });
            return;
        }
        sendStunRequest(address->toString(), port);
    });
}

void RendezvousService::sendStunRequest(const QString& host, quint16 port) {
    QHostAddress address(host);
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return;
    }
    stunTransactionId_.resize(12);
    for (qsizetype offset = 0; offset < stunTransactionId_.size(); offset += 4) {
        const auto value = QRandomGenerator::system()->generate();
        std::memcpy(stunTransactionId_.data() + offset, &value, sizeof(value));
    }
    QByteArray request;
    request.reserve(20);
    appendU16(request, 0x0001);
    appendU16(request, 0);
    appendU32(request, StunMagicCookie);
    request.append(stunTransactionId_);
    stunServerAddress_ = address;
    stunServerPort_ = port;
    socket_.writeDatagram(request, address, port);
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           QString("STUN discovery sent to %1:%2 from UDP %3")
                               .arg(host)
                               .arg(port)
                               .arg(localEndpoint_.port));
    const auto transaction = stunTransactionId_;
    const auto attempt = ++stunAttempts_;
    const auto quickAttempts = qMax(2, stunServers_.size() * 2);
    QTimer::singleShot(attempt <= quickAttempts ? 1500 : 60000, this,
                       [this, transaction] {
                           if (!running_ || stunTransactionId_ != transaction ||
                               stunEndpoint_.port != 0) {
                               return;
                           }
                           Logger::instance().log(
                               QtInfoMsg, "rendezvous",
                               QString("STUN response timeout from %1:%2")
                                   .arg(stunServerAddress_.toString())
                                   .arg(stunServerPort_));
                           ++stunServerIndex_;
                           startStunDiscovery();
                       });
}

bool RendezvousService::handleStunDatagram(const QByteArray& datagram,
                                           const QHostAddress& sender, quint16 senderPort) {
    if (datagram.size() < 20 || readU16(datagram, 0) != 0x0101 ||
        readU32(datagram, 4) != StunMagicCookie || datagram.mid(8, 12) != stunTransactionId_ ||
        sender != stunServerAddress_ || senderPort != stunServerPort_) {
        return false;
    }
    const auto bodyLength = readU16(datagram, 2);
    if (20 + bodyLength > datagram.size()) {
        return true;
    }
    qsizetype offset = 20;
    while (offset + 4 <= 20 + bodyLength) {
        const auto type = readU16(datagram, offset);
        const auto length = readU16(datagram, offset + 2);
        offset += 4;
        if (offset + length > datagram.size()) {
            return true;
        }
        if ((type == 0x0020 || type == 0x0001) && length >= 8 &&
            static_cast<uchar>(datagram.at(offset + 1)) == 0x01) {
            quint16 publicPort = readU16(datagram, offset + 2);
            quint32 publicAddress = readU32(datagram, offset + 4);
            if (type == 0x0020) {
                publicPort ^= static_cast<quint16>(StunMagicCookie >> 16);
                publicAddress ^= StunMagicCookie;
            }
            QHostAddress address(publicAddress);
            if (publicPort != 0 && isPublicIpv4(address)) {
                stunEndpoint_ = {address.toString(), publicPort};
                Logger::instance().log(
                    QtInfoMsg, "rendezvous",
                    QString("STUN observed endpoint: %1:%2")
                        .arg(stunEndpoint_.address)
                        .arg(stunEndpoint_.port));
            } else {
                Logger::instance().log(
                    QtInfoMsg, "rendezvous",
                    QString("STUN returned non-public or invalid endpoint %1:%2")
                        .arg(address.toString())
                        .arg(publicPort));
                return true;
            }
            if (!explicitMappingActive_) {
                publicEndpoint_ = stunEndpoint_;
                mappingMethod_ = "STUN";
                emit externalEndpointChanged(publicEndpoint_.address, publicEndpoint_.port,
                                             mappingMethod_);
                Logger::instance().log(
                    QtInfoMsg, "rendezvous",
                    QString("STUN endpoint discovered: %1:%2")
                        .arg(publicEndpoint_.address)
                        .arg(publicEndpoint_.port));
            }
            return true;
        }
        offset += (length + 3) & ~3;
    }
    return true;
}

QByteArray RendezvousService::encodeMessage(MessageType type, const QString& peerId,
                                            const QByteArray& contextId, quint16 chunkIndex,
                                            quint16 chunkCount, const QByteArray& payload,
                                            const QByteArray& forcedMessageId) const {
    const auto secret = secretForPeer(peerId);
    if (secret.size() != 32 || payload.size() > MaxChunkPayloadBytes) {
        return {};
    }
    const auto sender = idBytes(localIdentity_.peerId);
    const auto recipient = idBytes(peerId);
    const auto messageId = forcedMessageId.size() == 16 ? forcedMessageId : randomId();
    const auto context = contextId.size() == 16 ? contextId : QByteArray(16, '\0');
    if (sender.size() != 16 || recipient.size() != 16 || messageId.size() != 16) {
        return {};
    }

    QByteArray authenticated;
    QDataStream stream(&authenticated, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << WireMagic << WireVersion << static_cast<quint8>(type) << quint16{0};
    stream.writeRawData(sender.constData(), 16);
    stream.writeRawData(recipient.constData(), 16);
    stream << QDateTime::currentMSecsSinceEpoch();
    stream.writeRawData(messageId.constData(), 16);
    stream.writeRawData(context.constData(), 16);
    stream << chunkIndex << chunkCount << static_cast<quint16>(payload.size());
    authenticated.append(payload);
    const auto tag = QMessageAuthenticationCode::hash(authenticated, secret,
                                                       QCryptographicHash::Sha256);
    authenticated.append(tag);
    return authenticated;
}

std::optional<RendezvousService::DecodedMessage>
RendezvousService::decodeMessage(const QByteArray& datagram) {
    if (datagram.size() < WireHeaderBytes + AuthenticationTagBytes ||
        datagram.size() > MaxDatagramBytes) {
        return std::nullopt;
    }
    const auto authenticated = datagram.left(datagram.size() - AuthenticationTagBytes);
    QDataStream stream(authenticated);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint8 version = 0;
    quint8 rawType = 0;
    quint16 reserved = 0;
    QByteArray sender(16, '\0');
    QByteArray recipient(16, '\0');
    qint64 timestamp = 0;
    QByteArray messageId(16, '\0');
    QByteArray contextId(16, '\0');
    quint16 chunkIndex = 0;
    quint16 chunkCount = 0;
    quint16 payloadLength = 0;
    stream >> magic >> version >> rawType >> reserved;
    stream.readRawData(sender.data(), 16);
    stream.readRawData(recipient.data(), 16);
    stream >> timestamp;
    stream.readRawData(messageId.data(), 16);
    stream.readRawData(contextId.data(), 16);
    stream >> chunkIndex >> chunkCount >> payloadLength;
    if (stream.status() != QDataStream::Ok || magic != WireMagic || version != WireVersion ||
        reserved != 0 || idString(recipient) != normalizedPeerId(localIdentity_.peerId) ||
        rawType < static_cast<quint8>(MessageType::Probe) ||
        rawType > static_cast<quint8>(MessageType::SignalAck) ||
        WireHeaderBytes + payloadLength != authenticated.size() ||
        qAbs(QDateTime::currentMSecsSinceEpoch() - timestamp) > TimestampWindowMs) {
        return std::nullopt;
    }
    const auto senderId = idString(sender);
    auto contact = contacts_.find(senderId);
    if (contact == contacts_.end()) {
        return std::nullopt;
    }
    const auto expected = QMessageAuthenticationCode::hash(
        authenticated, contact->record.rendezvousSecret, QCryptographicHash::Sha256);
    if (!constantTimeEqual(expected, datagram.right(AuthenticationTagBytes))) {
        return std::nullopt;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto& replay = contact->replayCache;
    for (auto it = replay.begin(); it != replay.end();) {
        if (it.value() + ReplayRetentionMs < now) {
            it = replay.erase(it);
        } else {
            ++it;
        }
    }
    const bool duplicate = replay.contains(messageId);
    if (!duplicate) {
        if (replay.size() >= MaxReplayEntriesPerContact) {
            auto oldest = replay.begin();
            for (auto it = replay.begin(); it != replay.end(); ++it) {
                if (it.value() < oldest.value()) {
                    oldest = it;
                }
            }
            replay.erase(oldest);
        }
        replay.insert(messageId, now);
    }
    return DecodedMessage{static_cast<MessageType>(rawType), senderId, messageId, contextId,
                          chunkIndex, chunkCount,
                          authenticated.mid(WireHeaderBytes, payloadLength), duplicate};
}

void RendezvousService::processDatagram(const QByteArray& datagram, const QHostAddress& sender,
                                        quint16 senderPort) {
    if (handleStunDatagram(datagram, sender, senderPort)) {
        return;
    }
    const auto decoded = decodeMessage(datagram);
    if (decoded) {
        processMessage(*decoded, sender, senderPort);
    }
}

void RendezvousService::processMessage(const DecodedMessage& message,
                                       const QHostAddress& sender, quint16 senderPort) {
    auto contact = contacts_.find(message.senderId);
    if (contact == contacts_.end()) {
        return;
    }
    markAuthenticated(*contact, sender, senderPort);

    switch (message.type) {
    case MessageType::Probe: {
        const auto ack = encodeMessage(MessageType::ProbeAck, message.senderId,
                                       message.contextId, 0, 0, message.messageId);
        socket_.writeDatagram(ack, sender, senderPort);
        break;
    }
    case MessageType::ProbeAck:
        if (message.contextId == contact->probeAttemptId) {
            contact->probeStep = 0;
            contact->nextProbeMs = QDateTime::currentMSecsSinceEpoch() + withJitter(20000);
            Logger::instance().log(
                QtInfoMsg, "rendezvous",
                QString("Probe acknowledged by %1 from %2:%3")
                    .arg(contact->record.identity.displayName, sender.toString())
                    .arg(senderPort));
        }
        break;
    case MessageType::ConnectRequest: {
        const auto document = QJsonDocument::fromJson(message.payload);
        const auto meshId = document.object().value("mesh_id").toString();
        const auto requestId = idString(message.contextId);
        if (!document.isObject() || normalizedPeerId(meshId).isEmpty() || requestId.isEmpty()) {
            return;
        }
        const auto key = message.senderId + ':' + requestId;
        if (incomingRequests_.contains(key)) {
            const auto& existing = incomingRequests_.value(key);
            if (!existing.response.isEmpty()) {
                sendConnectResponse(existing);
            }
            return;
        }
        if (incomingRequests_.size() >= MaxIncomingRequests) {
            return;
        }
        IncomingRequest request{message.senderId, requestId, meshId,
                                QDateTime::currentMSecsSinceEpoch() + ConnectTimeoutMs, {}};
        incomingRequests_.insert(key, request);
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               "Connection request received from " +
                                   contact->record.identity.displayName);
        emit connectionRequestReceived(message.senderId, contact->record.identity.displayName,
                                       requestId, meshId);
        break;
    }
    case MessageType::ConnectResponse: {
        const auto requestId = idString(message.contextId);
        auto request = outgoingRequests_.find(requestId);
        const auto document = QJsonDocument::fromJson(message.payload);
        if (request == outgoingRequests_.end() || request->peerId != message.senderId ||
            !document.isObject()) {
            return;
        }
        const auto response = document.object().value("response").toString();
        const auto meshId = document.object().value("mesh_id").toString();
        if (response != "accepted" && response != "declined" && response != "busy") {
            return;
        }
        if (response == "accepted" &&
            (normalizedPeerId(meshId).isEmpty() || meshId != request->meshId)) {
            return;
        }
        contact->requestPending = false;
        setStatus(*contact, response == "busy" ? ContactStatus::Busy : ContactStatus::Online);
        emit contactChanged({contact->record, contact->status, false});
        outgoingRequests_.erase(request);
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               QString("Connection request %1 by %2")
                                   .arg(response, contact->record.identity.displayName));
        emit connectionResponseReceived(message.senderId, requestId, response, meshId);
        break;
    }
    case MessageType::Signal: {
        if (message.chunkCount == 0 || message.chunkCount > MaxSignalChunks ||
            message.chunkIndex >= message.chunkCount) {
            return;
        }
        const auto key = signalKey(message.senderId, message.contextId).toUtf8();
        auto assembly = incomingSignals_.find(key);
        if (assembly == incomingSignals_.end()) {
            if (incomingSignals_.size() >= MaxIncomingSignals) {
                return;
            }
            IncomingSignal incoming;
            incoming.peerId = message.senderId;
            incoming.chunkCount = message.chunkCount;
            incoming.expiresMs = QDateTime::currentMSecsSinceEpoch() + SignalTimeoutMs;
            assembly = incomingSignals_.insert(key, std::move(incoming));
        }
        if (assembly->chunkCount != message.chunkCount) {
            return;
        }
        if (!assembly->chunks.contains(message.chunkIndex)) {
            if (assembly->totalBytes + message.payload.size() > InvitationCodec::MaxBytes) {
                incomingSignals_.erase(assembly);
                return;
            }
            assembly->chunks.insert(message.chunkIndex, message.payload);
            assembly->totalBytes += message.payload.size();
        }
        const auto ack = encodeMessage(MessageType::SignalAck, message.senderId,
                                       message.contextId, message.chunkIndex,
                                       message.chunkCount, {});
        socket_.writeDatagram(ack, sender, senderPort);
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Signaling chunk %1/%2 received from %3")
                .arg(message.chunkIndex + 1)
                .arg(message.chunkCount)
                .arg(contact->record.identity.displayName));
        if (assembly->chunks.size() == assembly->chunkCount) {
            QByteArray document;
            document.reserve(assembly->totalBytes);
            for (int index = 0; index < assembly->chunkCount; ++index) {
                document.append(assembly->chunks.value(index));
            }
            incomingSignals_.erase(assembly);
            Logger::instance().log(QtInfoMsg, "rendezvous",
                                   "Signaling document reassembled from " +
                                       contact->record.identity.displayName);
            emit signalingReceived(message.senderId, document);
        }
        break;
    }
    case MessageType::SignalAck: {
        auto signal = outgoingSignals_.find(message.contextId);
        if (signal == outgoingSignals_.end() || signal->peerId != message.senderId ||
            message.chunkIndex >= signal->chunks.size()) {
            return;
        }
        if (!signal->acknowledged.contains(message.chunkIndex)) {
            signal->acknowledged.insert(message.chunkIndex);
            Logger::instance().log(
                QtInfoMsg, "rendezvous",
                QString("Signaling chunk %1/%2 acknowledged by %3")
                    .arg(message.chunkIndex + 1)
                    .arg(signal->chunks.size())
                    .arg(contact->record.identity.displayName));
        }
        if (signal->acknowledged.size() == signal->chunks.size()) {
            outgoingSignals_.erase(signal);
        }
        break;
    }
    }
}

void RendezvousService::sendToContact(const QString& peerId, const QByteArray& datagram) {
    const auto contact = contacts_.constFind(peerId);
    if (contact == contacts_.cend() || datagram.isEmpty()) {
        return;
    }
    sendToEndpoint(contact->record.publicEndpoint, datagram);
    if (contact->record.localEndpoint.address != contact->record.publicEndpoint.address ||
        contact->record.localEndpoint.port != contact->record.publicEndpoint.port) {
        sendToEndpoint(contact->record.localEndpoint, datagram);
    }
}

void RendezvousService::sendToEndpoint(const RendezvousEndpoint& endpoint,
                                       const QByteArray& datagram) {
    if (!endpoint.isValid() || endpoint.port == 0) {
        return;
    }
    QHostAddress address(endpoint.address);
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        socket_.writeDatagram(datagram, address, endpoint.port);
    }
}

void RendezvousService::sendProbe(ContactRuntime& contact, qint64 now) {
    if (contact.probeAttemptId.size() != 16 || contact.probeStep == 0) {
        contact.probeAttemptId = randomId();
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               "Probe started for " + contact.record.identity.displayName);
    }
    const auto message = encodeMessage(MessageType::Probe, contact.record.identity.peerId,
                                       contact.probeAttemptId, 0, 0, {});
    sendToContact(contact.record.identity.peerId, message);
    if (contact.probeStep < static_cast<int>(ProbeBurstMs.size()) - 1) {
        ++contact.probeStep;
        contact.nextProbeMs = now + withJitter(ProbeBurstMs.at(contact.probeStep));
        return;
    }
    setStatus(contact, ContactStatus::Offline);
    const auto offlineIndex = qMin(contact.probeStep - static_cast<int>(ProbeBurstMs.size()) + 1,
                                   static_cast<int>(OfflineRetryMs.size()) - 1);
    contact.nextProbeMs = now + withJitter(OfflineRetryMs.at(offlineIndex));
    ++contact.probeStep;
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           "Contact offline: " + contact.record.identity.displayName);
}

void RendezvousService::markAuthenticated(ContactRuntime& contact, const QHostAddress& sender,
                                          quint16 senderPort) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    contact.lastAuthenticatedMs = now;
    contact.record.lastSeen = QDateTime::currentDateTimeUtc();
    RendezvousEndpoint endpoint{sender.toString(), senderPort};
    auto& stored = sender.isPrivateUse() ? contact.record.localEndpoint
                                         : contact.record.publicEndpoint;
    const bool endpointChanged = stored.address != endpoint.address ||
                                 stored.port != endpoint.port;
    if (endpointChanged) {
        const auto old = stored;
        stored = endpoint;
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Endpoint changed for %1: %2:%3 -> %4:%5")
                .arg(contact.record.identity.displayName, old.address)
                .arg(old.port)
                .arg(endpoint.address)
                .arg(endpoint.port));
    }
    if (contact.status != ContactStatus::Connected) {
        setStatus(contact, ContactStatus::Online);
    }
    emit contactChanged({contact.record, contact.status, contact.requestPending});
    if (endpointChanged || contact.lastPersistedMs + 60000 <= now) {
        const auto saved = saveContacts();
        if (!saved) {
            Logger::instance().log(QtWarningMsg, "rendezvous", saved.error());
        } else {
            contact.lastPersistedMs = now;
        }
    }
}

void RendezvousService::setStatus(ContactRuntime& contact, ContactStatus status) {
    if (contact.status == status) {
        return;
    }
    contact.status = status;
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           QString("Contact %1 status: %2")
                               .arg(contact.record.identity.displayName,
                                    contactStatusName(status)));
    emit contactChanged({contact.record, contact.status, contact.requestPending});
}

QList<ContactPresence> RendezvousService::contacts() const {
    QList<ContactPresence> result;
    result.reserve(contacts_.size());
    for (const auto& contact : contacts_) {
        result.append({contact.record, contact.status, contact.requestPending});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.contact.identity.displayName.localeAwareCompare(
                   right.contact.identity.displayName) < 0;
    });
    return result;
}

std::optional<ContactRecord> RendezvousService::contact(const QString& peerId) const {
    const auto found = contacts_.constFind(normalizedPeerId(peerId));
    return found == contacts_.cend() ? std::nullopt
                                     : std::optional<ContactRecord>(found->record);
}

QByteArray RendezvousService::secretForPeer(const QString& peerId) const {
    const auto found = contacts_.constFind(normalizedPeerId(peerId));
    return found == contacts_.cend() ? QByteArray{} : found->record.rendezvousSecret;
}

Result<void> RendezvousService::rememberContact(ContactRecord record,
                                                bool allowSecretReplacement) {
    record.identity.peerId = normalizedPeerId(record.identity.peerId);
    if (!record.isValid() || record.identity.peerId == normalizedPeerId(localIdentity_.peerId)) {
        return Result<void>::failure("Cannot remember an invalid rendezvous contact");
    }
    auto existing = contacts_.find(record.identity.peerId);
    if (existing != contacts_.end()) {
        if (!allowSecretReplacement &&
            existing->record.rendezvousSecret != record.rendezvousSecret) {
            return Result<void>::failure("Rendezvous secret mismatch");
        }
        const auto previousStatus = existing->status;
        const auto previousSeen = existing->record.lastSeen;
        existing->record = std::move(record);
        if (!existing->record.lastSeen.isValid()) {
            existing->record.lastSeen = previousSeen;
        }
        existing->status = previousStatus;
        emit contactChanged({existing->record, existing->status, existing->requestPending});
    } else {
        ContactRuntime runtime;
        runtime.record = std::move(record);
        runtime.status = ContactStatus::Probing;
        runtime.nextProbeMs = QDateTime::currentMSecsSinceEpoch();
        existing = contacts_.insert(runtime.record.identity.peerId, std::move(runtime));
        emit contactChanged({existing->record, existing->status, false});
    }
    const auto saved = saveContacts();
    if (saved) {
        existing->lastPersistedMs = QDateTime::currentMSecsSinceEpoch();
        Logger::instance().log(QtInfoMsg, "rendezvous",
                               "Contact saved: " + existing->record.identity.displayName);
    }
    return saved;
}

void RendezvousService::setContactConnected(const QString& peerId, bool connected) {
    auto contact = contacts_.find(normalizedPeerId(peerId));
    if (contact == contacts_.end()) {
        return;
    }
    setStatus(*contact, connected ? ContactStatus::Connected : ContactStatus::Probing);
    if (!connected) {
        contact->probeStep = 0;
        contact->nextProbeMs = QDateTime::currentMSecsSinceEpoch();
    }
}

QString RendezvousService::requestConnection(const QString& peerId, const QString& meshId) {
    auto contact = contacts_.find(normalizedPeerId(peerId));
    if (contact == contacts_.end() || contact->status != ContactStatus::Online) {
        return {};
    }
    const auto requestId = idString(randomId());
    OutgoingRequest request{contact->record.identity.peerId, requestId,
                            normalizedPeerId(meshId),
                            QDateTime::currentMSecsSinceEpoch() + ConnectTimeoutMs,
                            QDateTime::currentMSecsSinceEpoch(), 0};
    outgoingRequests_.insert(requestId, request);
    contact->requestPending = true;
    emit contactChanged({contact->record, contact->status, true});
    sendConnectRequest(outgoingRequests_[requestId]);
    return requestId;
}

void RendezvousService::sendConnectRequest(OutgoingRequest& request) {
    const auto payload = QJsonDocument(QJsonObject{{"mesh_id", request.meshId}})
                             .toJson(QJsonDocument::Compact);
    const auto message = encodeMessage(MessageType::ConnectRequest, request.peerId,
                                       idBytes(request.requestId), 0, 0, payload);
    sendToContact(request.peerId, message);
    const auto retryIndex = qMin(request.attempt + 1,
                                 static_cast<int>(ConnectRetryMs.size()) - 1);
    request.nextSendMs = QDateTime::currentMSecsSinceEpoch() +
                         withJitter(ConnectRetryMs.at(retryIndex));
    ++request.attempt;
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           "Connection request sent to " + request.peerId.left(8));
}

void RendezvousService::respondToConnection(const QString& peerId, const QString& requestId,
                                            const QString& response, const QString& meshId) {
    const auto key = normalizedPeerId(peerId) + ':' + normalizedPeerId(requestId);
    auto request = incomingRequests_.find(key);
    if (request == incomingRequests_.end() ||
        (response != "accepted" && response != "declined" && response != "busy")) {
        return;
    }
    request->response = response;
    request->meshId = normalizedPeerId(meshId);
    sendConnectResponse(*request);
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           QString("Connection request %1 for %2")
                               .arg(response, peerId.left(8)));
}

void RendezvousService::sendConnectResponse(const IncomingRequest& request) {
    const auto payload = QJsonDocument(
                             QJsonObject{{"response", request.response},
                                         {"mesh_id", request.meshId}})
                             .toJson(QJsonDocument::Compact);
    const auto message = encodeMessage(MessageType::ConnectResponse, request.peerId,
                                       idBytes(request.requestId), 0, 0, payload);
    sendToContact(request.peerId, message);
}

Result<QString> RendezvousService::sendSignal(const QString& peerId,
                                              const QByteArray& document) {
    const auto normalized = normalizedPeerId(peerId);
    if (!contacts_.contains(normalized)) {
        return Result<QString>::failure("Unknown rendezvous contact");
    }
    if (document.isEmpty() || document.size() > InvitationCodec::MaxBytes) {
        return Result<QString>::failure("Signaling document is too large");
    }
    if (outgoingSignals_.size() >= MaxOutgoingSignals) {
        return Result<QString>::failure("Too many signaling objects are pending");
    }
    OutgoingSignal signal;
    signal.peerId = normalized;
    signal.signalId = randomId();
    signal.expiresMs = QDateTime::currentMSecsSinceEpoch() + SignalTimeoutMs;
    signal.nextSendMs = QDateTime::currentMSecsSinceEpoch();
    for (qsizetype offset = 0; offset < document.size(); offset += MaxChunkPayloadBytes) {
        signal.chunks.append(document.mid(offset, MaxChunkPayloadBytes));
    }
    if (signal.chunks.size() > MaxSignalChunks) {
        return Result<QString>::failure("Signaling document needs too many chunks");
    }
    const auto signalId = signal.signalId;
    outgoingSignals_.insert(signalId, std::move(signal));
    sendPendingSignal(outgoingSignals_[signalId]);
    return Result<QString>::success(idString(signalId));
}

void RendezvousService::sendPendingSignal(OutgoingSignal& signal) {
    for (int index = 0; index < signal.chunks.size(); ++index) {
        if (signal.acknowledged.contains(index)) {
            continue;
        }
        const auto message = encodeMessage(MessageType::Signal, signal.peerId, signal.signalId,
                                           static_cast<quint16>(index),
                                           static_cast<quint16>(signal.chunks.size()),
                                           signal.chunks.at(index));
        sendToContact(signal.peerId, message);
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Signaling chunk %1/%2 sent to %3")
                .arg(index + 1)
                .arg(signal.chunks.size())
                .arg(signal.peerId.left(8)));
    }
    const auto retryIndex = qMin(signal.attempt + 1,
                                 static_cast<int>(SignalRetryMs.size()) - 1);
    signal.nextSendMs = QDateTime::currentMSecsSinceEpoch() +
                        withJitter(SignalRetryMs.at(retryIndex));
    ++signal.attempt;
}

void RendezvousService::tick() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (auto& contact : contacts_) {
        if (contact.status == ContactStatus::Connected) {
            continue;
        }
        if (contact.lastAuthenticatedMs > 0 && now - contact.lastAuthenticatedMs > 60000 &&
            contact.status == ContactStatus::Online) {
            setStatus(contact, ContactStatus::Offline);
            contact.probeStep = static_cast<int>(ProbeBurstMs.size());
            contact.nextProbeMs = now + withJitter(30000);
        }
        if (now >= contact.nextProbeMs) {
            sendProbe(contact, now);
        }
    }

    for (auto it = outgoingRequests_.begin(); it != outgoingRequests_.end();) {
        if (now >= it->expiresMs) {
            auto contact = contacts_.find(it->peerId);
            if (contact != contacts_.end()) {
                contact->requestPending = false;
                emit contactChanged({contact->record, contact->status, false});
            }
            Logger::instance().log(QtInfoMsg, "rendezvous",
                                   "Connection request expired for " + it->peerId.left(8));
            it = outgoingRequests_.erase(it);
        } else {
            if (now >= it->nextSendMs) {
                sendConnectRequest(*it);
            }
            ++it;
        }
    }
    for (auto it = incomingRequests_.begin(); it != incomingRequests_.end();) {
        if (now >= it->expiresMs) {
            emit connectionRequestExpired(it->peerId, it->requestId);
            it = incomingRequests_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = outgoingSignals_.begin(); it != outgoingSignals_.end();) {
        if (now >= it->expiresMs) {
            Logger::instance().log(QtWarningMsg, "rendezvous",
                                   "Signaling delivery expired for " + it->peerId.left(8));
            it = outgoingSignals_.erase(it);
        } else {
            if (now >= it->nextSendMs) {
                sendPendingSignal(*it);
            }
            ++it;
        }
    }
    for (auto it = incomingSignals_.begin(); it != incomingSignals_.end();) {
        if (now >= it->expiresMs) {
            it = incomingSignals_.erase(it);
        } else {
            ++it;
        }
    }
}

Result<void> RendezvousService::saveContacts() {
    QList<ContactRecord> records;
    records.reserve(contacts_.size());
    for (const auto& contact : contacts_) {
        records.append(contact.record);
    }
    return store_.save(records);
}

RendezvousEndpoint RendezvousService::localEndpoint() const {
    return localEndpoint_;
}

RendezvousEndpoint RendezvousService::publicEndpoint() const {
    return publicEndpoint_;
}

QString RendezvousService::mappingMethod() const {
    return mappingMethod_;
}

QByteArray RendezvousService::randomId() {
    return QUuid::createUuid().toRfc4122();
}

QString RendezvousService::idString(const QByteArray& id) {
    if (id.size() != 16) {
        return {};
    }
    return QUuid::fromRfc4122(id).toString(QUuid::WithoutBraces).toLower();
}

QByteArray RendezvousService::idBytes(const QString& id) {
    const QUuid parsed(id);
    return parsed.isNull() ? QByteArray{} : parsed.toRfc4122();
}

} // namespace tmc
