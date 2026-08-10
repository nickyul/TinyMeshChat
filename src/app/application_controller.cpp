#include "tmc/app/application_controller.h"

#include "tmc/core/logger.h"
#include "tmc/identity/identity_manager.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace tmc {

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
    if (!QFile::exists(configPath) && !QFile::copy(":/default-config.json", configPath)) {
        return Result<InitializationState>::failure("Cannot create the user configuration: " +
                                                    configPath);
    }
    auto config = AppConfig::load(configPath);
    if (!config) {
        return Result<InitializationState>::failure(config.error());
    }
    config_ = config.value();
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

} // namespace tmc
