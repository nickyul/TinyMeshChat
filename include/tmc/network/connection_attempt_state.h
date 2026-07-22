#pragma once

#include <QMetaType>
#include <QString>

namespace tmc {

enum class ConnectionAttemptState {
    Gathering,
    AwaitingAnswer,
    AwaitingConnection,
    Connecting,
    AwaitingHello,
    Connected,
    Suspect,
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
    case ConnectionAttemptState::AwaitingHello:
        return "awaiting-hello";
    case ConnectionAttemptState::Connected:
        return "connected";
    case ConnectionAttemptState::Suspect:
        return "suspect";
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
