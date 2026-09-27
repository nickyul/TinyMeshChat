#pragma once

#include "tmc/app/connection_manager.h"
#include "tmc/protocol/packet.h"
#include "tmc/transfers/file_transfer_service.h"
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

namespace tmc {
class ApplicationController;

// Independent authenticated P2P connections for personal transfers. They never
// join, create or leave a mesh and can coexist with a group conversation.
class PersonalConnections final : public QObject {
    Q_OBJECT
public:
    using SignalSender = std::function<bool(const QString&, const QString&, const QString&, const QString&)>;
    PersonalConnections(ApplicationController& app, SignalSender sender, QObject* parent = nullptr);
    Result<void> sendFile(const QString& peerId, const QString& path);
    void receiveSignal(const QString& peerId, const QString& connectionId, const QString& kind, const QString& sdp);
    void setTurnServers(const QList<RelayServer>& servers, int expiresInSeconds);
    FileTransferService* files() { return &files_; }
private:
    PeerIdentity knownPeer(const QString& peerId) const;
    void begin(const PeerIdentity& peer);
    void send(const QString& id, PacketType type, PacketPayload payload);
    void sendProof(const QString& id);
    void receiveControl(const QString& id, const QString& text);
    void maintain();
    ApplicationController& app_;
    SignalSender sender_;
    ConnectionManager connections_;
    FileTransferService files_;
    QTimer timer_;
    QElapsedTimer clock_;
    QHash<QString, QString> nonces_;
    QHash<QString, HelloPayload> hellos_;
    QHash<QString, qint64> retryAt_;
    QHash<QString, qint64> idleAt_;
    QHash<QString, QString> pingNonces_;
};
} // namespace tmc
