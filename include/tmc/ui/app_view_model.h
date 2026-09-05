#pragma once

#include "tmc/core/app_config.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <memory>

namespace tmc {

class ApplicationController;
class AppLinkController;
class NetworkSession;
class UpdateService;
class MessagesModel;
class PeersModel;
class ContactsModel;
class GlobalPttMonitor;

class AppViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool identityRequired READ identityRequired NOTIFY identityRequiredChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool meshVisible READ meshVisible NOTIFY meshStateChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY meshStateChanged)
    Q_PROPERTY(bool degraded READ degraded NOTIFY meshStateChanged)
    Q_PROPERTY(int connectedPeerCount READ connectedPeerCount NOTIFY meshPeerCountsChanged)
    Q_PROPERTY(int expectedPeerCount READ expectedPeerCount NOTIFY meshPeerCountsChanged)
    Q_PROPERTY(bool callActive READ callActive NOTIFY callStateChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callStateChanged)
    Q_PROPERTY(bool invitationPending READ invitationPending NOTIFY invitationStateChanged)
    Q_PROPERTY(QString invitationState READ invitationState NOTIFY invitationStateChanged)
    Q_PROPERTY(QString stunServersText READ stunServersText NOTIFY stunServersChanged)

    // Audio properties
    Q_PROPERTY(QStringList captureDevices READ captureDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QStringList playbackDevices READ playbackDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString captureDevice READ captureDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString playbackDevice READ playbackDevice NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool echoCancellation READ echoCancellation NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool highPassFilter READ highPassFilter NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool noiseSuppression READ noiseSuppression NOTIFY audioSettingsChanged)
    Q_PROPERTY(int noiseSuppressionLevel READ noiseSuppressionLevel NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool automaticGainControl READ automaticGainControl NOTIFY audioSettingsChanged)
    Q_PROPERTY(int agcTargetLevelDbfs READ agcTargetLevelDbfs NOTIFY audioSettingsChanged)
    Q_PROPERTY(int agcCompressionGainDb READ agcCompressionGainDb NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool agcLimiter READ agcLimiter NOTIFY audioSettingsChanged)
    Q_PROPERTY(int outputVolume READ outputVolume NOTIFY audioSettingsChanged)
    Q_PROPERTY(int qualityKbps READ qualityKbps NOTIFY audioSettingsChanged)
    Q_PROPERTY(int inputMode READ inputMode NOTIFY audioSettingsChanged)
    Q_PROPERTY(double vadThreshold READ vadThreshold NOTIFY audioSettingsChanged)
    Q_PROPERTY(int vadHangoverMs READ vadHangoverMs NOTIFY audioSettingsChanged)
    Q_PROPERTY(QString pttBindingName READ pttBindingName NOTIFY pttBindingChanged)
    Q_PROPERTY(bool pttBindingCapturing READ pttBindingCapturing NOTIFY pttBindingChanged)
    Q_PROPERTY(bool pttPressed READ pttPressed NOTIFY pttPressedChanged)
    Q_PROPERTY(bool isTalking READ isTalking NOTIFY talkingStateChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool microphoneTest READ microphoneTest NOTIFY audioSettingsChanged)
    Q_PROPERTY(double microphoneLevel READ microphoneLevel NOTIFY microphoneLevelChanged)
    Q_PROPERTY(bool appLinksRegistered READ appLinksRegistered NOTIFY appLinksRegisteredChanged)
    Q_PROPERTY(QString updateState READ updateState NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateReleaseNotes READ updateReleaseNotes NOTIFY updateStateChanged)
    Q_PROPERTY(int updateProgress READ updateProgress NOTIFY updateProgressChanged)
    Q_PROPERTY(bool updaterPortable READ updaterPortable NOTIFY updateStateChanged)
    Q_PROPERTY(bool incomingContactRequest READ incomingContactRequest
                   NOTIFY incomingContactRequestChanged)
    Q_PROPERTY(QString incomingContactName READ incomingContactName
                   NOTIFY incomingContactRequestChanged)
    Q_PROPERTY(QAbstractItemModel* messages READ messages CONSTANT)
    Q_PROPERTY(QAbstractItemModel* peers READ peers CONSTANT)
    Q_PROPERTY(QAbstractItemModel* contacts READ contacts CONSTANT)

public:
    AppViewModel(ApplicationController& controller, AppLinkController& appLinks,
                 UpdateService& updates,
                 bool identityRequired, QObject* parent = nullptr);
    ~AppViewModel() override;

    bool identityRequired() const;
    QString displayName() const;
    QString status() const;
    bool meshVisible() const;
    bool connecting() const;
    bool degraded() const;
    int connectedPeerCount() const;
    int expectedPeerCount() const;
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
    bool highPassFilter() const;
    bool noiseSuppression() const;
    int noiseSuppressionLevel() const;
    bool automaticGainControl() const;
    int agcTargetLevelDbfs() const;
    int agcCompressionGainDb() const;
    bool agcLimiter() const;
    int outputVolume() const;
    int qualityKbps() const;
    int inputMode() const;
    double vadThreshold() const;
    int vadHangoverMs() const;
    QString pttBindingName() const;
    bool pttBindingCapturing() const;
    bool pttPressed() const;
    bool isTalking() const;
    bool deafened() const;
    bool microphoneTest() const;
    double microphoneLevel() const;
    bool appLinksRegistered() const;
    QString updateState() const;
    QString updateVersion() const;
    QString updateReleaseNotes() const;
    int updateProgress() const;
    bool updaterPortable() const;
    bool incomingContactRequest() const;
    QString incomingContactName() const;
    QAbstractItemModel* messages() const;
    QAbstractItemModel* peers() const;
    QAbstractItemModel* contacts() const;

    Q_INVOKABLE void createIdentity(const QString& displayName);
    Q_INVOKABLE void updateDisplayName(const QString& displayName);
    Q_INVOKABLE void updateStunServers(const QString& servers);
    Q_INVOKABLE void createMesh();
    Q_INVOKABLE void leaveMesh();
    Q_INVOKABLE void createInvitation();
    Q_INVOKABLE void cancelInvitation();
    Q_INVOKABLE void recreateInvitation();
    Q_INVOKABLE void importSignalingText(const QString& text);
    Q_INVOKABLE void importSignalingFile(const QUrl& url);
    Q_INVOKABLE void saveSignalingFile(const QUrl& url);
    Q_INVOKABLE void copyText(const QString& text);
    Q_INVOKABLE bool sendMessage(const QString& text);
    Q_INVOKABLE void toggleCall();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleDeafen();
    Q_INVOKABLE void toggleMicrophoneTest();
    Q_INVOKABLE void refreshAudioDevices();
    Q_INVOKABLE void prepareAudioSettings();
    Q_INVOKABLE void cancelAudioSettingsEdit();
    Q_INVOKABLE void beginPttBindingCapture();
    Q_INVOKABLE void updateAudioPreferences(const QVariantMap& settings);
    Q_INVOKABLE void setPeerVolume(const QString& peerId, int percent);
    Q_INVOKABLE void registerAppLinks();
    Q_INVOKABLE void unregisterAppLinks();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void connectContact(const QString& peerId);
    Q_INVOKABLE void acceptIncomingContact();
    Q_INVOKABLE void declineIncomingContact();
    Q_INVOKABLE QString diagnostics() const;

    void checkForUpdatesAutomatically();

signals:
    void identityRequiredChanged();
    void displayNameChanged();
    void statusChanged();
    void meshStateChanged();
    void meshPeerCountsChanged();
    void callStateChanged();
    void invitationStateChanged();
    void stunServersChanged();
    void audioDevicesChanged();
    void audioSettingsChanged();
    void microphoneLevelChanged();
    void pttPressedChanged();
    void pttBindingChanged();
    void talkingStateChanged();
    void appLinksRegisteredChanged();
    void updateStateChanged();
    void updateProgressChanged();
    void incomingContactRequestChanged();
    void contactConnectionNotification(QString message);
    void updatePromptRequested();
    void errorRequested(QString message);
    void signalingRequested(QString kind, QString text, QString link);

private:
    void initializeSession();
    void setMeshPeerCounts(int connected, int expected);
    void setStatus(const QString& status);
    void reportError(const QString& error);
    void showNextContactRequest();

    void updateMicrophoneLevel();
    void updateMeterTimerState();
    void updatePttMonitorState();
    void setPttPressed(bool pressed);

    ApplicationController& controller_;
    AppLinkController& appLinks_;
    UpdateService& updates_;
    std::unique_ptr<NetworkSession> session_;
    std::unique_ptr<MessagesModel> messages_;
    std::unique_ptr<PeersModel> peers_;
    std::unique_ptr<ContactsModel> contacts_;
    std::unique_ptr<GlobalPttMonitor> pttMonitor_;
    QByteArray signalingDocument_;
    QString status_;
    QStringList captureDevices_;
    QStringList playbackDevices_;
    double microphoneLevel_{0.0};
    int connectedPeerCount_{0};
    int expectedPeerCount_{0};
    bool identityRequired_{false};
    bool pttPressed_{false};
    bool isTalking_{false};
    PttBinding pendingPttBinding_;

    struct IncomingContactRequest {
        QString peerId;
        QString displayName;
        QString requestId;
        QString meshId;
    };
    QList<IncomingContactRequest> incomingContactRequests_;
    IncomingContactRequest activeContactRequest_;

    QTimer meterTimer_;
};

} // namespace tmc
