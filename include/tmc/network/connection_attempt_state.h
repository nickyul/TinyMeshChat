#pragma once

#include <QMetaType>
#include <QString>

namespace tmc {

enum class ConnectionAttemptState {
    Gathering,
    AwaitingAnswer,
    AwaitingConnection,
    Connecting,
    Connected,
    TimedOut,
    Failed,
    Cancelled
};

inline QString toString(ConnectionAttemptState state) {
    switch (state) {
    case ConnectionAttemptState::Gathering:
        return "gathering";
    case ConnectionAttemptState::AwaitingAnswer:
        return "awaiting-answer";
    case ConnectionAttemptState::AwaitingConnection:
        return "awaiting-connection";
    case ConnectionAttemptState::Connecting:
        return "connecting";
    case ConnectionAttemptState::Connected:
        return "connected";
    case ConnectionAttemptState::TimedOut:
        return "timed-out";
    case ConnectionAttemptState::Failed:
        return "failed";
    case ConnectionAttemptState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ConnectionAttemptState)
