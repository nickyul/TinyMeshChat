#include "tmc/app/application_controller.h"

#include "tmc/core/logger.h"
#include "tmc/identity/identity_manager.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace tmc {

namespace {

constexpr quint16 MinRendezvousPort = 49152;
constexpr int RendezvousPortCount = 3;

quint16 randomRendezvousPort() {
    return static_cast<quint16>(QRandomGenerator::system()->bounded(
        static_cast<quint32>(MinRendezvousPort), static_cast<quint32>(65536)));
}

} // namespace

ApplicationController::ApplicationController(QObject* p) : QObject(p) {
}

const PeerIdentity& ApplicationController::identity() const {
    return identity_;
}

const AppConfig& ApplicationController::config() const {
    return config_;
}

const ConnectionPolicy& ApplicationController::connectionPolicy() const {
    return connectionPolicy_;
}

QString ApplicationController::dataDirectory() const {
    return dataDir_;
}

Result<InitializationState> ApplicationController::initialize() {
    dataDir_ = qEnvironmentVariable("TMC_DATA_DIR");
    if (dataDir_.isEmpty()) {
        dataDir_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (!QDir().mkpath(dataDir_)) {
        return Result<InitializationState>::failure("Cannot create application data directory: " +
                                                    dataDir_);
    }
    Logger::instance().setFilePath(dataDir_ + "/debug.log");
    const auto configPath = dataDir_ + "/config.json";
    if (!QFile::exists(configPath)) {
        if (!QFile::copy(":/default-config.json", configPath)) {
            return Result<InitializationState>::failure(
                "Cannot create the user configuration: " + configPath);
        }
        const auto permissions = QFile::permissions(configPath);
        if (!QFile::setPermissions(configPath, permissions | QFileDevice::WriteOwner)) {
            return Result<InitializationState>::failure(
                "Cannot make the user configuration writable: " + configPath);
        }
    }
    auto config = AppConfig::load(configPath);
    if (!config) {
        return Result<InitializationState>::failure(config.error());
    }
    config_ = config.value();
    if (config_.rendezvous.localPorts.empty()) {
        while (config_.rendezvous.localPorts.size() < RendezvousPortCount) {
            const auto port = randomRendezvousPort();
            if (!config_.rendezvous.localPorts.contains(port)) {
                config_.rendezvous.localPorts.append(port);
            }
        }
        const auto saved = config_.save(configPath);
        if (!saved) {
            return Result<InitializationState>::failure(saved.error());
        }
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Generated persistent rendezvous ports: %1, %2, %3")
                .arg(config_.rendezvous.localPorts.at(0))
                .arg(config_.rendezvous.localPorts.at(1))
                .arg(config_.rendezvous.localPorts.at(2)));
    }
    identityPath_ = dataDir_ + "/identity.json";
    if (!QFile::exists(identityPath_)) {
        return Result<InitializationState>::success(InitializationState::DisplayNameRequired);
    }

    auto identity = IdentityManager::load(identityPath_);
    if (!identity) {
        return Result<InitializationState>::failure(identity.error());
    }
    identity_ = identity.value();
    Logger::instance().log(QtInfoMsg, "app",
                           "Application initialized for peer " + identity_.peerId.left(8));
    return Result<InitializationState>::success(InitializationState::Ready);
}

Result<void> ApplicationController::createIdentity(const QString& displayName) {
    if (identityPath_.isEmpty()) {
        return Result<void>::failure("Application is not initialized");
    }
    auto identity = IdentityManager::create(identityPath_, displayName);
    if (!identity) {
        return Result<void>::failure(identity.error());
    }
    identity_ = identity.value();
    Logger::instance().log(QtInfoMsg, "app",
                           "Identity created for peer " + identity_.peerId.left(8));
    return Result<void>::success();
}

Result<void> ApplicationController::updateDisplayName(const QString& displayName) {
    if (identityPath_.isEmpty() || !identity_.isValid()) {
        return Result<void>::failure("Identity is not initialized");
    }
    auto identity = IdentityManager::updateDisplayName(identityPath_, identity_, displayName);
    if (!identity) {
        return Result<void>::failure(identity.error());
    }
    if (identity_.displayName == identity.value().displayName) {
        return Result<void>::success();
    }
    identity_ = identity.value();
    emit displayNameChanged(identity_.displayName);
    Logger::instance().log(QtInfoMsg, "identity", "Display name updated");
    return Result<void>::success();
}

Result<void> ApplicationController::updateStunServers(const QStringList& servers) {
    AppConfig updated = config_;
    updated.stunServers.clear();
    for (const auto& server : servers) {
        const auto normalized = server.trimmed();
        if (!normalized.isEmpty() && !updated.stunServers.contains(normalized)) {
            updated.stunServers.append(normalized);
        }
    }
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = updated;
    emit stunServersChanged(config_.stunServers);
    Logger::instance().log(QtInfoMsg, "config", "STUN server list updated");
    return Result<void>::success();
}

Result<void> ApplicationController::updateAudioPreferences(const AudioPreferences& preferences) {
    if (!preferences.isValid()) {
        return Result<void>::failure("Некорректные настройки аудио.");
    }
    AppConfig updated = config_;
    updated.audio = preferences;
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = updated;
    emit audioPreferencesChanged(config_.audio);
    Logger::instance().log(QtInfoMsg, "config", "Audio preferences updated");
    return Result<void>::success();
}

Result<void> ApplicationController::updateRendezvousPorts(const QList<quint16>& ports,
                                                          quint16 boundPort) {
    AppConfig updated = config_;
    updated.rendezvous.localPorts = ports;
    updated.rendezvous.lastBoundPort = boundPort;
    if (!updated.rendezvous.isValid()) {
        return Result<void>::failure("The replacement rendezvous ports are invalid");
    }
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = std::move(updated);
    return Result<void>::success();
}

Result<void> ApplicationController::updateRendezvousExternalEndpoint(
    const QString& address, quint16 port, const QString& mappingMethod) {
    if (address.trimmed().isEmpty() || port == 0) {
        return Result<void>::failure("The rendezvous external endpoint is invalid");
    }
    if (config_.rendezvous.lastPublicAddress == address &&
        config_.rendezvous.lastPublicPort == port &&
        config_.rendezvous.mappingMethod == mappingMethod) {
        return Result<void>::success();
    }
    AppConfig updated = config_;
    updated.rendezvous.lastPublicAddress = address;
    updated.rendezvous.lastPublicPort = port;
    updated.rendezvous.mappingMethod = mappingMethod;
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = std::move(updated);
    return Result<void>::success();
}

} // namespace tmc
