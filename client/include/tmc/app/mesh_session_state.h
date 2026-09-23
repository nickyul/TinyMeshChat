#pragma once

#include <QMetaType>
#include <QString>

namespace tmc {

enum class MeshSessionState { Disconnected, Connecting, InMesh, Degraded };

inline QString toString(MeshSessionState state) {
    switch (state) {
    case MeshSessionState::Disconnected:
        return "disconnected";
    case MeshSessionState::Connecting:
        return "connecting";
    case MeshSessionState::InMesh:
        return "in-mesh";
    case MeshSessionState::Degraded:
        return "degraded";
    }
    return "unknown";
}

} // namespace tmc

Q_DECLARE_METATYPE(tmc::MeshSessionState)
