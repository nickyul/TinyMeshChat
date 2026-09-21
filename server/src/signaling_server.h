#pragma once

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QWebSocketServer>

class QWebSocket;

namespace tmc::server {

class SignalingServer final : public QObject {
public:
    explicit SignalingServer(QObject* parent = nullptr);
    ~SignalingServer() override;

    bool listen(const QHostAddress& address, quint16 port);
    void close();

    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QHostAddress serverAddress() const;
    [[nodiscard]] quint16 serverPort() const;

private:
    void acceptPendingConnections();
    void registerClient(QWebSocket* socket);

    QWebSocketServer server_;
    QSet<QWebSocket*> clients_;
};

}  // namespace tmc::server
