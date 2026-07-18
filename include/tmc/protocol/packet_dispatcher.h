#pragma once

#include "tmc/protocol/packet.h"

#include <QString>

#include <functional>
#include <map>

namespace tmc {

struct PacketContext {
    QString connectionId;
};

class PacketDispatcher {
public:
    using Handler = std::function<void(const PacketContext&, const Packet&)>;

    void registerHandler(PacketType type, Handler handler);
    bool dispatch(const PacketContext& context, const Packet& packet) const;

private:
    std::map<PacketType, Handler> handlers_;
};

} // namespace tmc
