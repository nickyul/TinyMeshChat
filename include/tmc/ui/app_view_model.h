#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QUrl>

#include <memory>

namespace tmc {

class ApplicationController;
class NetworkSession;
class MessagesModel;
class PeersModel;

class AppViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool identityRequired READ identityRequired NOTIFY identityRequiredChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool meshVisible READ meshVisible NOTIFY meshStateChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY meshStateChanged)
    Q_PROPERTY(bool degraded READ degraded NOTIFY meshStateChanged)
    Q_PROPERTY(QString meshSummary READ meshSummary NOTIFY meshSummaryChanged)
    Q_PROPERTY(bool callActive READ callActive NOTIFY callStateChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callStateChanged)
    Q_PROPERTY(QString stunServersText READ stunServersText NOTIFY stunServersChanged)
    Q_PROPERTY(QAbstractItemModel* messages READ messages CONSTANT)
    Q_PROPERTY(QAbstractItemModel* peers READ peers CONSTANT)

public:
    AppViewModel(ApplicationController& controller, bool identityRequired,
                 QObject* parent = nullptr);
    ~AppViewModel() override;

    bool identityRequired() const;
    QString displayName() const;
    QString status() const;
    bool meshVisible() const;
    bool connecting() const;
    bool degraded() const;
    QString meshSummary() const;
    bool callActive() const;
    bool muted() const;
    QString stunServersText() const;
    QAbstractItemModel* messages() const;
    QAbstractItemModel* peers() const;

    Q_INVOKABLE void createIdentity(const QString& displayName);
    Q_INVOKABLE void updateDisplayName(const QString& displayName);
    Q_INVOKABLE void updateStunServers(const QString& servers);
    Q_INVOKABLE void createMesh();
    Q_INVOKABLE void leaveMesh();
    Q_INVOKABLE void createInvitation();
    Q_INVOKABLE void importSignalingText(const QString& text);
    Q_INVOKABLE void importSignalingFile(const QUrl& url);
    Q_INVOKABLE void saveSignalingFile(const QUrl& url);
    Q_INVOKABLE void copyText(const QString& text);
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void toggleCall();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE QString diagnostics() const;

signals:
    void identityRequiredChanged();
    void displayNameChanged();
    void statusChanged();
    void meshStateChanged();
    void meshSummaryChanged();
    void callStateChanged();
    void stunServersChanged();
    void errorRequested(QString message);
    void signalingRequested(QString kind, QString text, QString suggestedName);

private:
    void initializeSession();
    void setStatus(const QString& status);
    void reportError(const QString& error);

    ApplicationController& controller_;
    std::unique_ptr<NetworkSession> session_;
    std::unique_ptr<MessagesModel> messages_;
    std::unique_ptr<PeersModel> peers_;
    QByteArray signalingDocument_;
    QString signalingName_;
    QString status_;
    QString meshSummary_{"Прямые связи: 0/0"};
    bool identityRequired_{false};
};

} // namespace tmc
