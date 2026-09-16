#pragma once

#include "tmc/core/app_config.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/network/connection_policy.h"

#include <QObject>

namespace tmc {

enum class InitializationState { Ready, DisplayNameRequired };

class ApplicationController : public QObject {
    Q_OBJECT

public:
    explicit ApplicationController(QObject* p = nullptr);

    Result<InitializationState> initialize();

    Result<void> createIdentity(const QString& displayName);
    Result<void> updateDisplayName(const QString& displayName);

    const PeerIdentity& identity() const;
    const AppConfig& config() const;
    const ConnectionPolicy& connectionPolicy() const;
    QString dataDirectory() const;

    Result<void> updateStunServers(const QStringList& servers);
    Result<void> updateAudioPreferences(const AudioPreferences& preferences);
    Result<void> updateRendezvousPorts(const QList<quint16>& ports, quint16 boundPort);
    Result<void> updateRendezvousExternalEndpoint(const QString& address, quint16 port,
                                                  const QString& mappingMethod);

signals:
    void fatalError(QString);
    void displayNameChanged(QString displayName);
    void stunServersChanged(QStringList servers);
    void audioPreferencesChanged(tmc::AudioPreferences preferences);

private:
    QString dataDir_;
    QString identityPath_;
    PeerIdentity identity_;
    AppConfig config_;
    ConnectionPolicy connectionPolicy_;
};

} // namespace tmc
