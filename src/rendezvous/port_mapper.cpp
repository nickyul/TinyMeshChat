#include "tmc/rendezvous/port_mapper.h"

#include "tmc/core/logger.h"

#include <QMetaObject>
#include <QPointer>

#include <plum/plum.h>

namespace tmc {

QString portMappingMethodName(PortMappingMethod method) {
    switch (method) {
    case PortMappingMethod::Pcp:
        return "PCP";
    case PortMappingMethod::NatPmp:
        return "NAT-PMP";
    case PortMappingMethod::Upnp:
        return "UPnP";
    case PortMappingMethod::Direct:
        return "Direct";
    case PortMappingMethod::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

namespace {

PortMappingMethod mappingMethod(int method) {
    switch (method) {
    case PLUM_MAPPING_PROTOCOL_PCP:
        return PortMappingMethod::Pcp;
    case PLUM_MAPPING_PROTOCOL_NATPMP:
        return PortMappingMethod::NatPmp;
    case PLUM_MAPPING_PROTOCOL_UPNP:
        return PortMappingMethod::Upnp;
    case PLUM_MAPPING_PROTOCOL_DIRECT:
        return PortMappingMethod::Direct;
    default:
        return PortMappingMethod::Unavailable;
    }
}

void plumLog(plum_log_level_t level, const char* message) {
    if (level < PLUM_LOG_LEVEL_WARN || !message) {
        return;
    }
    Logger::instance().log(level >= PLUM_LOG_LEVEL_ERROR ? QtWarningMsg : QtInfoMsg,
                           "port-mapping", QString::fromUtf8(message));
}

} // namespace

LibPlumPortMapper::LibPlumPortMapper(QObject* parent) : IPortMapper(parent) {
}

LibPlumPortMapper::~LibPlumPortMapper() {
    stop();
}

void LibPlumPortMapper::start(quint16 internalPort, quint16 preferredExternalPort) {
    if (initialized_ && mappingId_ >= 0 && internalPort_ == internalPort &&
        preferredExternalPort_ == preferredExternalPort) {
        Logger::instance().log(
            QtInfoMsg, "port-mapping",
            "Mapping recovery/renewal remains active in libplum after network change");
        return;
    }
    stop();
    Logger::instance().log(
        QtInfoMsg, "port-mapping",
        QString("Starting PCP/NAT-PMP/UPnP discovery for local UDP %1")
            .arg(internalPort));
    plum_config_t config{};
    config.log_level = PLUM_LOG_LEVEL_WARN;
    config.log_callback = &plumLog;
    config.discover_timeout = 5000;
    config.mapping_timeout = 5000;
    config.recheck_period = 60000;
    config.protocol = PLUM_PROTOCOL_ANY;
    const auto initialized = plum_init(&config);
    if (initialized != PLUM_ERR_SUCCESS) {
        emit mappingFailed(QString("libplum initialization failed (%1)").arg(initialized));
        return;
    }
    initialized_ = true;
    internalPort_ = internalPort;
    preferredExternalPort_ = preferredExternalPort;

    plum_mapping_t mapping{};
    mapping.protocol = PLUM_IP_PROTOCOL_UDP;
    mapping.internal_port = internalPort;
    mapping.external_port = preferredExternalPort;
    mapping.user_ptr = this;
    mappingId_ = plum_create_mapping(
        &mapping, +[](int id, plum_state_t state, const plum_mapping_t* mapping) {
            if (!mapping || !mapping->user_ptr) {
                return;
            }
            auto* mapper = static_cast<LibPlumPortMapper*>(mapping->user_ptr);
            const QPointer<LibPlumPortMapper> guard(mapper);
            const auto address = QString::fromUtf8(mapping->external_host);
            const auto port = mapping->external_port;
            const auto method = static_cast<int>(mapping->mapping_protocol);
            QMetaObject::invokeMethod(
                mapper,
                [guard, id, state, address, port, method] {
                    if (guard) {
                        guard->processMappingState(id, state, address, port, method);
                    }
                },
                Qt::QueuedConnection);
        });
    if (mappingId_ < 0) {
        const auto error = mappingId_;
        plum_cleanup();
        initialized_ = false;
        internalPort_ = 0;
        preferredExternalPort_ = 0;
        mappingId_ = -1;
        emit mappingFailed(QString("libplum mapping request failed (%1)").arg(error));
        return;
    }
    Logger::instance().log(
        QtInfoMsg, "port-mapping",
        QString("Requested UDP mapping: internal %1, preferred external %2")
            .arg(internalPort)
            .arg(preferredExternalPort));
}

void LibPlumPortMapper::stop() {
    if (mappingId_ >= 0) {
        plum_destroy_mapping(mappingId_);
        mappingId_ = -1;
    }
    if (initialized_) {
        plum_cleanup();
        initialized_ = false;
    }
    internalPort_ = 0;
    preferredExternalPort_ = 0;
}

void LibPlumPortMapper::processMappingState(int id, int state, QString address, quint16 port,
                                            int method) {
    if (id != mappingId_) {
        return;
    }
    if (state == PLUM_STATE_SUCCESS) {
        const auto mappedMethod = mappingMethod(method);
        Logger::instance().log(
            QtInfoMsg, "port-mapping",
            QString("Mapping %1 ready at %2:%3; lease renewal is managed by libplum")
                .arg(portMappingMethodName(mappedMethod), address)
                .arg(port));
        emit mappingReady({std::move(address), port, mappedMethod});
    } else if (state == PLUM_STATE_FAILURE) {
        emit mappingFailed("No PCP, NAT-PMP or UPnP UDP mapping is available");
    }
}

} // namespace tmc
