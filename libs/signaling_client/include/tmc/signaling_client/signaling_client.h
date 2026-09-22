#pragma once

#include "tmc/signaling_protocol/envelope.h"
#include "tmc/security/security.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <memory>

class QWebSocket;

namespace tmc {

// WebSocket transport and request correlation. No mesh or WebRTC ownership.
class SignalingClient final : public QObject {
    Q_OBJECT
public:
    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient() override;
    static bool validServerUrl(const QUrl& url);
    void connectTo(const QUrl& url);
    void configureAccess(std::shared_ptr<security::SigningKey> key, QJsonObject grants);
    void redeemAccess(const QUrl& url, const QString& authority, const QString& token);
    void stop();
    QString request(const QString& type, const QJsonObject& body = {});
    bool ready() const;
    QString state() const;
    QUrl url() const;
    QString sessionId() const;

signals:
    void stateChanged();
    void readyChanged();
    void connectionLost();
    void responseReceived(QString requestType, tmc::signaling_protocol::Envelope response);
    void eventReceived(tmc::signaling_protocol::Envelope event);
    void requestFailed(QString requestId, QString requestType, QString code);
    void accessRequired(QString reason);
    void accessGranted(QString server, QJsonObject grant);

private:
    struct Pending { QString type; qint64 sentAt; };
    void receive(const QString& text);
    void requireAccess(const QString& reason);
    void fail();
    void resetSocket();
    void openSocket();
    std::unique_ptr<QWebSocket> socket_;
    QHash<QString, Pending> pending_;
    QTimer timer_;
    QTimer reconnectTimer_;
    QElapsedTimer clock_;
    QUrl url_;
    QString state_{QStringLiteral("disabled")};
    QString sessionId_;
    int retryAttempt_{0};
    bool reconnectEnabled_{false};
    qint64 readyAt_{0};
    qint64 openedAt_{0};
    qint64 pingAt_{0};
    QByteArray pendingPing_;
    quint64 nextRequest_{0};
    std::shared_ptr<security::SigningKey> signingKey_;
    QJsonObject grants_;
    QString expectedAuthority_;
    QString accessToken_;
};

} // namespace tmc
