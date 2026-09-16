#pragma once

#include "tmc/core/app_config.h"
#include "tmc/core/result.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/rendezvous/contact_store.h"
#include "tmc/rendezvous/port_mapper.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>

#include <memory>
#include <optional>

namespace tmc {

enum class ContactStatus { Probing, Online, Offline, Busy, Connected };

QString contactStatusName(ContactStatus status);

struct ContactPresence {
    ContactRecord contact;
    ContactStatus status{ContactStatus::Probing};
    bool requestPending{false};
};

class RendezvousService final : public QObject {
    Q_OBJECT

public:
    RendezvousService(PeerIdentity localIdentity, RendezvousPreferences preferences,
                      QStringList stunServers, QString contactsPath,
                      std::unique_ptr<IPortMapper> portMapper = {}, QObject* parent = nullptr);
    ~RendezvousService() override;

    Result<void> start();
    void stop();
    void restartDiscovery();

    QList<ContactPresence> contacts() const;
    std::optional<ContactRecord> contact(const QString& peerId) const;
    QByteArray secretForPeer(const QString& peerId) const;
    Result<void> rememberContact(ContactRecord contact, bool allowSecretReplacement);
    void setContactConnected(const QString& peerId, bool connected);

    QString requestConnection(const QString& peerId, const QString& meshId);
    void respondToConnection(const QString& peerId, const QString& requestId,
                             const QString& response, const QString& meshId);
    Result<QString> sendSignal(const QString& peerId, const QByteArray& document);

    RendezvousEndpoint localEndpoint() const;
    RendezvousEndpoint publicEndpoint() const;
    QString mappingMethod() const;

signals:
    void persistentPortsChanged(QList<quint16> ports, quint16 boundPort);
    void externalEndpointChanged(QString address, quint16 port, QString mappingMethod);
    void contactChanged(tmc::ContactPresence contact);
    void connectionRequestReceived(QString peerId, QString displayName, QString requestId,
                                   QString meshId);
    void connectionResponseReceived(QString peerId, QString requestId, QString response,
                                    QString meshId);
    void connectionRequestExpired(QString peerId, QString requestId);
    void signalingReceived(QString peerId, QByteArray document);

private:
    enum class MessageType : quint8 {
        Probe = 1,
        ProbeAck = 2,
        ConnectRequest = 3,
        ConnectResponse = 4,
        Signal = 5,
        SignalAck = 6,
    };

    struct DecodedMessage {
        MessageType type{};
        QString senderId;
        QByteArray messageId;
        QByteArray contextId;
        quint16 chunkIndex{0};
        quint16 chunkCount{0};
        QByteArray payload;
        bool duplicate{false};
    };

    struct ContactRuntime {
        ContactRecord record;
        ContactStatus status{ContactStatus::Probing};
        QByteArray probeAttemptId;
        qint64 lastAuthenticatedMs{0};
        qint64 nextProbeMs{0};
        int probeStep{0};
        bool requestPending{false};
        qint64 lastPersistedMs{0};
        QHash<QByteArray, qint64> replayCache;
    };

    struct OutgoingRequest {
        QString peerId;
        QString requestId;
        QString meshId;
        qint64 expiresMs{0};
        qint64 nextSendMs{0};
        int attempt{0};
    };

    struct IncomingRequest {
        QString peerId;
        QString requestId;
        QString meshId;
        qint64 expiresMs{0};
        QString response;
    };

    struct OutgoingSignal {
        QString peerId;
        QByteArray signalId;
        QList<QByteArray> chunks;
        QSet<int> acknowledged;
        qint64 expiresMs{0};
        qint64 nextSendMs{0};
        int attempt{0};
    };

    struct IncomingSignal {
        QString peerId;
        int chunkCount{0};
        QHash<int, QByteArray> chunks;
        qsizetype totalBytes{0};
        qint64 expiresMs{0};
    };

    bool bindSocket();
    void startEndpointDiscovery();
    void startStunDiscovery();
    void sendStunRequest(const QString& host, quint16 port);
    bool handleStunDatagram(const QByteArray& datagram, const QHostAddress& sender,
                            quint16 senderPort);

    QByteArray encodeMessage(MessageType type, const QString& peerId,
                             const QByteArray& contextId, quint16 chunkIndex,
                             quint16 chunkCount, const QByteArray& payload,
                             const QByteArray& forcedMessageId = {}) const;
    std::optional<DecodedMessage> decodeMessage(const QByteArray& datagram);
    void processDatagram(const QByteArray& datagram, const QHostAddress& sender,
                         quint16 senderPort);
    void processMessage(const DecodedMessage& message, const QHostAddress& sender,
                        quint16 senderPort);
    void sendToContact(const QString& peerId, const QByteArray& datagram);
    void sendToEndpoint(const RendezvousEndpoint& endpoint, const QByteArray& datagram);
    void sendProbe(ContactRuntime& contact, qint64 now);
    void sendConnectRequest(OutgoingRequest& request);
    void sendConnectResponse(const IncomingRequest& request);
    void sendPendingSignal(OutgoingSignal& signal);

    void markAuthenticated(ContactRuntime& contact, const QHostAddress& sender,
                           quint16 senderPort);
    void setStatus(ContactRuntime& contact, ContactStatus status);
    void tick();
    Result<void> saveContacts();
    static QByteArray randomId();
    static QString idString(const QByteArray& id);
    static QByteArray idBytes(const QString& id);

    PeerIdentity localIdentity_;
    RendezvousPreferences preferences_;
    QStringList stunServers_;
    ContactStore store_;
    std::unique_ptr<IPortMapper> portMapper_;
    QUdpSocket socket_;
    QTimer timer_;
    QHash<QString, ContactRuntime> contacts_;
    QHash<QString, OutgoingRequest> outgoingRequests_;
    QHash<QString, IncomingRequest> incomingRequests_;
    QHash<QByteArray, OutgoingSignal> outgoingSignals_;
    QHash<QByteArray, IncomingSignal> incomingSignals_;
    RendezvousEndpoint localEndpoint_;
    RendezvousEndpoint publicEndpoint_;
    RendezvousEndpoint stunEndpoint_;
    QString mappingMethod_{"unavailable"};
    bool explicitMappingActive_{false};
    QByteArray stunTransactionId_;
    QHostAddress stunServerAddress_;
    quint16 stunServerPort_{0};
    int stunServerIndex_{0};
    int stunAttempts_{0};
    bool running_{false};
};

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ContactPresence)
