#pragma once

#include "tmc/signaling_protocol/envelope.h"
#include "tmc/signaling_protocol/turn_credentials.h"
#include "tmc/security/security.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <variant>

class QWebSocket;

namespace tmc {

// Server connection, authentication, request correlation and reconnects.
// No mesh or WebRTC ownership.
class SignalingClient final : public QObject {
    Q_OBJECT
public:
    enum class RequestError {
        NotReady,
        TooManyPendingRequests,
        InvalidMessage,
        MessageTooLarge,
        OutgoingQueueFull,
        SendFailed,
    };
    using RequestResult = std::variant<QString, RequestError>;

    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient() override;
    static bool validServerUrl(const QUrl& url);
    bool connectTo(const QUrl& url);
    void configureAccess(std::shared_ptr<security::SigningKey> key, QJsonObject grants);
    void redeemAccess(const QUrl& url, const QString& authority, const QString& token);
    void stop();
    [[nodiscard]] RequestResult request(const QString& type, const QJsonObject& body = {});
    bool ready() const;
    QString state() const;
    QUrl url() const;
    QString sessionId() const;
    signaling_protocol::TurnCredentials turnCredentials() const;

signals:
    void stateChanged();
    void readyChanged();
    void connectionLost();
    void responseReceived(QString requestType, tmc::signaling_protocol::Envelope response);
    void eventReceived(tmc::signaling_protocol::Envelope event);
    void requestFailed(QString requestId, QString requestType, QString code);
    void accessRequired(QString reason);
    void accessGranted(QString server, QJsonObject grant);
    void turnCredentialsChanged();

private:
    enum class State {
        Disabled,
        Connecting,
        Authenticating,
        Ready,
        Reconnecting,
        AccessRequired,
    };

    struct Pending {
        QString type;
        qint64 sentAt;
    };

    void receive(const QString& text);
    void handleChallenge(const signaling_protocol::Envelope& message);
    void handleResponse(const signaling_protocol::Envelope& message);
    void handleAuthenticationResponse(const QString& requestType,
                                      const signaling_protocol::Envelope& message);

    void requireAccess(const QString& reason);
    void fail();
    void resetSocket();
    void openSocket();
    void maintainConnection();
    void maintainConnectionAttempt(qint64 now);
    void maintainReadyConnection(qint64 now);
    void maintainHeartbeat(qint64 now);
    void maintainTurnCredentials(qint64 now);
    void updateTurnCredentials(const QJsonObject& body);
    [[nodiscard]] bool hasTimedOutRequest(qint64 now) const;

    // Connection and lifecycle timers.
    std::unique_ptr<QWebSocket> socket_;
    QTimer maintenanceTimer_;
    QElapsedTimer clock_;
    QUrl url_;
    State state_{State::Disabled};
    QString sessionId_;
    qint64 openedAt_{0};

    // Reconnect backoff; reset after a stable connection.
    QTimer reconnectTimer_;
    int retryAttempt_{0};
    bool reconnectEnabled_{false};
    qint64 readyAt_{0};

    // Request correlation and timeouts.
    QHash<QString, Pending> pending_;
    quint64 nextRequest_{0};

    // Heartbeat.
    qint64 pingAt_{0};
    QByteArray pendingPing_;

    // Identity, saved grants and one-use access invitation.
    std::shared_ptr<security::SigningKey> signingKey_;
    QJsonObject grants_;
    QString expectedAuthority_;
    QString accessToken_;

    // Server-issued TURN access, kept only for this signaling connection.
    signaling_protocol::TurnCredentials turn_;
    qint64 turnExpiresAt_{0};
    qint64 turnRefreshAt_{0};
    QString turnRefreshRequestId_;
};

} // namespace tmc
