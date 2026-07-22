#pragma once

namespace tmc {

class NetworkSession;
class PacketDispatcher;
struct Packet;
struct PacketContext;

class SessionPacketHandlers {
public:
    explicit SessionPacketHandlers(NetworkSession& session);

    void registerWith(PacketDispatcher& dispatcher);

private:
    void handleMembership(const PacketContext& context, const Packet& packet);
    void handleMessaging(const PacketContext& context, const Packet& packet);
    void handleVoice(const PacketContext& context, const Packet& packet);
    void handleRouting(const PacketContext& context, const Packet& packet);
    void handleMeshSignaling(const PacketContext& context, const Packet& packet);
    void handleSessionSignaling(const PacketContext& context, const Packet& packet);
    void handleHeartbeat(const PacketContext& context, const Packet& packet);

    NetworkSession& session_;
};

} // namespace tmc
