#pragma once
#include <QString>
namespace tmc {
enum class ConnectionState {
    Disconnected,
    Gathering,
    WaitingForOffer,
    WaitingForAnswer,
    Connecting,
    Connected,
    Failed
};
inline QString toString(ConnectionState s) {
    switch (s) {
    case ConnectionState::Disconnected:
        return "Disconnected";
    case ConnectionState::Gathering:
        return "Gathering";
    case ConnectionState::WaitingForOffer:
        return "WaitingForOffer";
    case ConnectionState::WaitingForAnswer:
        return "WaitingForAnswer";
    case ConnectionState::Connecting:
        return "Connecting";
    case ConnectionState::Connected:
        return "Connected";
    case ConnectionState::Failed:
        return "Failed";
    }
    return "Unknown";
}
} // namespace tmc
