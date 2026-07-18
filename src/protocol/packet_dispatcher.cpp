#include "tmc/protocol/packet_dispatcher.h"

#include <utility>

namespace tmc {

void PacketDispatcher::registerHandler(PacketType type, Handler handler) {
    handlers_[type] = std::move(handler);
}

bool PacketDispatcher::dispatch(const PacketContext& context, const Packet& packet) const {
    const auto handler = handlers_.find(packet.type);
    if (handler == handlers_.end()) {
        return false;
    }
    handler->second(context, packet);
    return true;
}

} // namespace tmc
