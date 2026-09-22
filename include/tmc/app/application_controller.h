#pragma once

#include "tmc/core/app_config.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/network/connection_policy.h"

#include <QObject>
#include <QList>
#include "tmc/security/security.h"

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
    const QList<PeerIdentity>& acquaintances() const;
    std::shared_ptr<security::SigningKey> signingKey() const;
    QJsonObject serverAccess() const;
    Result<void> saveServerAccess(const QString& url, const QJsonObject& grant);
    Result<void> rememberAcquaintance(const PeerIdentity& peer);

    Result<void> updateSignalingServer(const QString& url);
    Result<void> updateStunServers(const QStringList& servers);
    Result<void> updateAudioPreferences(const AudioPreferences& preferences);

signals:
    void fatalError(QString);
    void displayNameChanged(QString displayName);
    void acquaintancesChanged();
    void stunServersChanged(QStringList servers);
    void audioPreferencesChanged(tmc::AudioPreferences preferences);

private:
    QString dataDir_;
    QString identityPath_;
    PeerIdentity identity_;
    QList<PeerIdentity> acquaintances_;
    std::shared_ptr<security::SigningKey> signingKey_;
    QJsonObject serverAccess_;
    AppConfig config_;
    ConnectionPolicy connectionPolicy_;
};

} // namespace tmc
