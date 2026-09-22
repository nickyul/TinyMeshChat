#pragma once

#include "room_registry.h"
#include "presence_registry.h"
#include "tmc/security/security.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWebSocketProtocol>
#include <QWebSocketServer>

class QWebSocket;

namespace tmc::server {

class SignalingServer final : public QObject {
public:
    explicit SignalingServer(std::shared_ptr<security::SigningKey> authority, QObject* parent = nullptr);
    ~SignalingServer() override;

    bool listen(const QHostAddress& address, quint16 port);
    void close();

    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QHostAddress serverAddress() const;
    [[nodiscard]] quint16 serverPort() const;

private:
    struct Session {
        QString id;
        QSet<QString> requestIds;
        double requestBudget{40};
        qint64 budgetUpdatedAt{0};
        qint64 lastPingAt{0};
        QByteArray pendingPing;
        QString nonce;
        QString identityId;
        qint64 openedAt{0};
    };

    void acceptPendingConnections();
    void registerClient(QWebSocket* socket);
    void receive(QWebSocket* socket, const QString& text);
    bool send(QWebSocket* socket, const signaling_protocol::Envelope& message);
    void deliver(const QVector<Delivery>& deliveries);
    void removeClient(QWebSocket* socket);
    void closeClient(QWebSocket* socket, QWebSocketProtocol::CloseCode code,
                     const QString& reason);
    void maintainConnections();
    bool handleAccess(QWebSocket* socket, const signaling_protocol::Envelope& request);
    [[nodiscard]] QWebSocket* socketForSession(const QString& sessionId) const;

    QWebSocketServer server_;
    QSet<QWebSocket*> sockets_; // Includes sockets completing the close handshake.
    QHash<QWebSocket*, Session> clients_;
    QQueue<QVector<Delivery>> pendingDeliveries_;
    bool delivering_{false};
    RoomRegistry rooms_;
    PresenceRegistry presence_;
    QElapsedTimer clock_;
    QTimer maintenance_;
    std::shared_ptr<security::SigningKey> authority_;
    struct AccessInvitation { QString issuerSession; qint64 expiresAt; };
    QHash<QString, AccessInvitation> accessInvitations_;
};

}  // namespace tmc::server
