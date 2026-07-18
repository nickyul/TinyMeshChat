#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <memory>

namespace tmc {

class ApplicationController;
class AppLinkController;
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
    Q_PROPERTY(bool invitationPending READ invitationPending NOTIFY invitationStateChanged)
    Q_PROPERTY(QString invitationState READ invitationState NOTIFY invitationStateChanged)
    Q_PROPERTY(QString stunServersText READ stunServersText NOTIFY stunServersChanged)
    Q_PROPERTY(QStringList captureDevices READ captureDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QStringList playbackDevices READ playbackDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString captureDevice READ captureDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString playbackDevice READ playbackDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool echoCancellation READ echoCancellation NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool noiseSuppression READ noiseSuppression NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool automaticGainControl READ automaticGainControl NOTIFY audioSettingsChanged)
    Q_PROPERTY(int outputVolume READ outputVolume NOTIFY audioSettingsChanged)
    Q_PROPERTY(int qualityKbps READ qualityKbps NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool microphoneTest READ microphoneTest NOTIFY audioSettingsChanged)
    Q_PROPERTY(double microphoneLevel READ microphoneLevel NOTIFY microphoneLevelChanged)
    Q_PROPERTY(bool appLinksRegistered READ appLinksRegistered NOTIFY appLinksRegisteredChanged)
    Q_PROPERTY(QAbstractItemModel* messages READ messages CONSTANT)
    Q_PROPERTY(QAbstractItemModel* peers READ peers CONSTANT)

public:
    AppViewModel(ApplicationController& controller, AppLinkController& appLinks,
                 bool identityRequired, QObject* parent = nullptr);
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
    bool invitationPending() const;
    QString invitationState() const;
    QString stunServersText() const;
    QStringList captureDevices() const;
    QStringList playbackDevices() const;
    QString captureDevice() const;
    QString playbackDevice() const;
    bool echoCancellation() const;
    bool noiseSuppression() const;
    bool automaticGainControl() const;
    int outputVolume() const;
    int qualityKbps() const;
    bool deafened() const;
    bool microphoneTest() const;
    double microphoneLevel() const;
    bool appLinksRegistered() const;
    QAbstractItemModel* messages() const;
    QAbstractItemModel* peers() const;

    Q_INVOKABLE void createIdentity(const QString& displayName);
    Q_INVOKABLE void updateDisplayName(const QString& displayName);
    Q_INVOKABLE void updateStunServers(const QString& servers);
    Q_INVOKABLE void createMesh();
    Q_INVOKABLE void leaveMesh();
    Q_INVOKABLE void createInvitation();
    Q_INVOKABLE void cancelInvitation();
    Q_INVOKABLE void recreateInvitation();
    Q_INVOKABLE void importSignalingText(const QString& text);
    Q_INVOKABLE void previewSignalingLink(const QString& text);
    Q_INVOKABLE void confirmPendingSignaling();
    Q_INVOKABLE void importSignalingFile(const QUrl& url);
    Q_INVOKABLE void saveSignalingFile(const QUrl& url);
    Q_INVOKABLE void copyText(const QString& text);
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void toggleCall();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleDeafen();
    Q_INVOKABLE void toggleMicrophoneTest();
    Q_INVOKABLE void refreshAudioDevices();
    Q_INVOKABLE void updateAudioPreferences(const QString& captureDevice,
                                            const QString& playbackDevice, bool echoCancellation,
                                            bool noiseSuppression, bool automaticGainControl,
                                            int outputVolume, int qualityKbps);
    Q_INVOKABLE void setPeerVolume(const QString& peerId, int percent);
    Q_INVOKABLE void registerAppLinks();
    Q_INVOKABLE void unregisterAppLinks();
    Q_INVOKABLE QString diagnostics() const;

signals:
    void identityRequiredChanged();
    void displayNameChanged();
    void statusChanged();
    void meshStateChanged();
    void meshSummaryChanged();
    void callStateChanged();
    void invitationStateChanged();
    void stunServersChanged();
    void audioDevicesChanged();
    void audioSettingsChanged();
    void microphoneLevelChanged();
    void appLinksRegisteredChanged();
    void errorRequested(QString message);
    void signalingRequested(QString kind, QString text, QString link, QString suggestedName);
    void signalingPreviewRequested(QString kind, QString peerName, QString expiresAt);

private:
    void initializeSession();
    void setStatus(const QString& status);
    void reportError(const QString& error);

    ApplicationController& controller_;
    AppLinkController& appLinks_;
    std::unique_ptr<NetworkSession> session_;
    std::unique_ptr<MessagesModel> messages_;
    std::unique_ptr<PeersModel> peers_;
    QByteArray signalingDocument_;
    QString signalingName_;
    QString status_;
    QString meshSummary_{"Прямые связи: 0/0"};
    QStringList captureDevices_;
    QStringList playbackDevices_;
    double microphoneLevel_{0.0};
    QString pendingSignalingText_;
    bool identityRequired_{false};
};

} // namespace tmc
