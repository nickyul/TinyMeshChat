#include "app/application_controller.h"
#include "core/logger.h"
#include "identity/identity_manager.h"
#include <QDir>
#include <QFile>
#include <QStandardPaths>
using namespace tmc;
ApplicationController::ApplicationController(QObject* p) : QObject(p) {
}
Result<void> ApplicationController::initialize(const QString& name) {
    dataDir_ = qEnvironmentVariable("TMC_DATA_DIR");
    if (dataDir_.isEmpty())
        dataDir_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dataDir_))
        return Result<void>::failure("Cannot create application data directory: " + dataDir_);
    const auto configPath = dataDir_ + "/config.json";
    if (!QFile::exists(configPath) && !QFile::copy(":/default-config.json", configPath))
        return Result<void>::failure("Cannot create the user configuration: " + configPath);
    auto config = AppConfig::load(configPath);
    if (!config)
        return Result<void>::failure(config.error());
    config_ = config.value();
    auto i = IdentityManager::loadOrCreate(dataDir_ + "/identity.json", name);
    if (!i)
        return Result<void>::failure(i.error());
    identity_ = i.value();
    Logger::instance().log(QtInfoMsg, "app",
                           "Application initialized for peer " + identity_.peerId.left(8));
    return Result<void>::success();
}

Result<void> ApplicationController::updateStunServers(const QStringList& servers) {
    AppConfig updated = config_;
    updated.stunServers.clear();
    for (const auto& server : servers) {
        const auto normalized = server.trimmed();
        if (!normalized.isEmpty() && !updated.stunServers.contains(normalized))
            updated.stunServers.append(normalized);
    }
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved)
        return saved;
    config_ = updated;
    Logger::instance().log(QtInfoMsg, "config", "STUN server list updated");
    return Result<void>::success();
}
