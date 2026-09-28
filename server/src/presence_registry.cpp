#include "tmc/server/presence_registry.h"
#include "tmc/server/limits.h"
#include "tmc/signaling_protocol/message_codec.h"
#include <QUuid>
#include <algorithm>

namespace tmc::server {
using namespace signaling_protocol;
namespace {
Envelope event(const QString& type, const QJsonObject& body) { return {signaling_protocol::ProtocolVersion, type, std::nullopt, body}; }
}

bool PresenceRegistry::mutual(const QString& first, const QString& second) const {
    const auto a = profiles_.constFind(first), b = profiles_.constFind(second);
    return a != profiles_.cend() && b != profiles_.cend() && first != second &&
           a->known.contains(b->identityId) && b->known.contains(a->identityId);
}

QVector<Delivery> PresenceRegistry::finish(const Invitation& invitation, const QString& status) const {
    return {{invitation.fromSession, event("contact.result", {{"invitationId", invitation.id},
                {"toIdentityId", invitation.toIdentity}, {"status", status}})},
            {invitation.toSession, event("contact.closed", {{"invitationId", invitation.id}, {"status", status}})}};
}

QVector<Delivery> PresenceRegistry::finishKnock(const Invitation& request, const QString& status) const {
    return {{request.fromSession, event("knock.result", {{"invitationId", request.id},
                {"toIdentityId", request.toIdentity}, {"status", status}})},
            {request.toSession, event("knock.closed", {{"invitationId", request.id}, {"status", status}})}};
}

QVector<Delivery> PresenceRegistry::handle(const QString& sessionId, const Envelope& request,
                                          RoomRegistry& rooms, qint64 now) {
    const auto fail = [&](const QString& code) -> QVector<Delivery> {
        return {{sessionId, MessageCodec::error(request.requestId, code)}};
    };
    const auto reply = [&](const QString& type, const QJsonObject& body = {}) -> Delivery {
        return {sessionId, {signaling_protocol::ProtocolVersion, type, request.requestId, body}};
    };
    if (MessageCodec::validateRequest(request)) return fail("invalid_message");
    const auto& body = request.body;
    if (request.type == "presence.publish") {
        const auto identity = body.value("identityId").toString();
        if ((sessionsByIdentity_.contains(identity) && sessionsByIdentity_.value(identity) != sessionId) ||
            (profiles_.contains(sessionId) && profiles_.value(sessionId).identityId != identity))
            return fail("identity_in_use");
        if (!profiles_.contains(sessionId) && profiles_.size() >= MaxSessions) return fail("resource_limit");
        auto& profile = profiles_[sessionId];
        profile.identityId = identity;
        profile.displayName = body.value("displayName").toString();
        profile.busy = body.value("busy").toBool();
        profile.meshId = body.value("meshId").toString();
        profile.known.clear();
        for (const auto& item : body.value("knownPeers").toArray()) profile.known.insert(item.toString());
        sessionsByIdentity_.insert(identity, sessionId);
        auto deliveries = QVector<Delivery>{reply("presence.published")};
        deliveries += maintainInvitations(rooms, now);
        deliveries += updateSnapshots();
        return deliveries;
    }
    if (!profiles_.contains(sessionId)) return fail("presence_required");
    if (request.type == "mesh.leave") {
        auto& profile = profiles_[sessionId];
        const auto meshId = body.value("meshId").toString();
        if (profile.meshId != meshId) return fail("not_in_mesh");
        profile.meshId.clear();
        auto deliveries = QVector<Delivery>{reply("mesh.left")};
        for (auto peer = profiles_.cbegin(); peer != profiles_.cend(); ++peer) {
            if (peer.key() != sessionId && peer->meshId == meshId)
                deliveries.append({peer.key(), event("mesh.departed", {{"meshId", meshId}, {"identityId", profile.identityId}})});
        }
        deliveries += maintainInvitations(rooms, now);
        deliveries += updateSnapshots();
        return deliveries;
    }
    if (request.type == "direct.send") {
        const auto target = sessionsByIdentity_.value(body.value("identityId").toString());
        if (!mutual(sessionId, target)) return fail("recipient_unavailable");
        auto forwarded = body;
        forwarded.insert("identityId", profiles_.value(sessionId).identityId);
        return {{target, event("direct.received", forwarded)}, reply("direct.accepted")};
    }
    if (request.type == "mesh.relay") {
        const auto target = sessionsByIdentity_.value(body.value("identityId").toString());
        const auto meshId = body.value("meshId").toString();
        if (profiles_.value(sessionId).meshId != meshId) return fail("not_in_mesh");
        if (target.isEmpty() || target == sessionId || !profiles_.contains(target)) return fail("recipient_unavailable");
        if (profiles_.value(target).meshId != meshId) return fail("recipient_left");
        return {{target, event("mesh.received", {{"meshId", meshId},
                    {"identityId", profiles_.value(sessionId).identityId}, {"payload", body.value("payload")}})},
                reply("mesh.relayed")};
    }
    if (request.type == "knock.request") {
        const auto identity = body.value("toIdentityId").toString();
        const auto target = sessionsByIdentity_.value(identity);
        if (!mutual(sessionId, target)) return fail("recipient_unavailable");
        if (profiles_.value(sessionId).busy || !rooms.roomForSession(sessionId).isEmpty()) return fail("busy");
        const auto mesh = profiles_.value(target).meshId;
        if (mesh.isEmpty()) return fail("not_in_mesh");
        for (const auto& knock : knocks_)
            if (knock.fromSession == sessionId || knock.toSession == target) return fail("busy");
        if (knocks_.size() >= 64) return fail("resource_limit");
        auto& profile = profiles_[sessionId];
        if (profile.lastInvite.contains(identity) && now - profile.lastInvite.value(identity) < 10000)
            return fail("rate_limited");
        profile.lastInvite.insert(identity, now);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        knocks_.insert(id, {id, sessionId, target, identity, {}, mesh, now + 60000});
        return {reply("knock.requested", {{"invitationId", id}}),
            {target, event("knock.invitation", {{"invitationId", id}, {"fromIdentityId", profile.identityId},
                {"displayName", profile.displayName}, {"meshId", mesh}, {"expiresInSeconds", 60}})}};
    }
    if (request.type == "knock.respond") {
        const auto id = body.value("invitationId").toString();
        auto found = knocks_.find(id);
        if (found == knocks_.end() || found->toSession != sessionId) return fail("invitation_unavailable");
        const auto knock = found.value();
        knocks_.erase(found);
        QVector<Delivery> deliveries{reply("knock.responded")};
        QString status;
        const auto decision = body.value("decision").toString();
        const auto room = body.value("roomId").toString();
        if (now >= knock.expiresAt) status = "expired";
        else if (!mutual(knock.fromSession, sessionId) || profiles_.value(sessionId).meshId != knock.meshId) status = "cancelled";
        else if (profiles_.value(knock.fromSession).busy || !rooms.roomForSession(knock.fromSession).isEmpty() || decision == "busy") status = "busy";
        else if (decision == "decline") status = "declined";
        else if (rooms.roomForSession(sessionId) != room || !rooms.canJoinRoom(room)) status = "unavailable";
        else {
            const auto result = rooms.createInvitation(sessionId, room, now);
            if (const auto* issued = std::get_if<RoomRegistry::CreatedInvitation>(&result)) {
                deliveries.append({knock.fromSession, event("knock.accepted", {{"invitationId", id},
                    {"roomId", room}, {"meshId", knock.meshId}, {"token", issued->token}})});
                status = "accepted";
            } else status = "unavailable";
        }
        deliveries += finishKnock(knock, status);
        return deliveries;
    }
    if (request.type == "contact.invite") {
        const auto identity = body.value("toIdentityId").toString();
        const auto target = sessionsByIdentity_.value(identity);
        if (!mutual(sessionId, target)) return fail("recipient_unavailable");
        const auto room = body.value("roomId").toString();
        if (rooms.roomForSession(sessionId) != room) return fail("not_in_room");
        if (!rooms.canJoinRoom(room)) return fail("room_full");
        if (profiles_.value(target).busy || !rooms.roomForSession(target).isEmpty()) return fail("busy");
        int outgoing = 0;
        for (const auto& pending : invitations_) {
            if (pending.toSession == target) return fail("busy");
            if (pending.fromSession == sessionId) ++outgoing;
        }
        if (outgoing >= RoomCapacity - 1 || invitations_.size() >= 64) return fail("resource_limit");
        auto& profile = profiles_[sessionId];
        if (profile.lastInvite.contains(identity) && now - profile.lastInvite.value(identity) < 10000)
            return fail("rate_limited");
        profile.lastInvite.insert(identity, now);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Invitation invitation{id, sessionId, target, identity, room, body.value("meshId").toString(),
                              now + OnlineInvitationLifetimeSeconds * qint64{1000}};
        invitations_.insert(id, invitation);
        return {reply("contact.invited", {{"invitationId", id}}),
            {target, event("contact.invitation", {{"invitationId", id}, {"fromIdentityId", profile.identityId},
                {"displayName", profile.displayName}, {"meshId", invitation.meshId},
                {"expiresInSeconds", OnlineInvitationLifetimeSeconds}})}};
    }
    if (request.type != "contact.respond") return fail("invalid_message");
    const auto id = body.value("invitationId").toString();
    auto found = invitations_.find(id);
    if (found == invitations_.end() || found->toSession != sessionId) return fail("invitation_unavailable");
    const auto invitation = found.value();
    invitations_.erase(found); // A decision is one-shot, including failed acceptance.
    QVector<Delivery> deliveries{reply("contact.responded")};
    QString status;
    const auto decision = body.value("decision").toString();
    if (now >= invitation.expiresAt) status = "expired";
    else if (!mutual(invitation.fromSession, sessionId) ||
             rooms.roomForSession(invitation.fromSession) != invitation.roomId) status = "cancelled";
    else if (profiles_.value(sessionId).busy || !rooms.roomForSession(sessionId).isEmpty() || decision == "busy") status = "busy";
    else if (decision == "decline") status = "declined";
    else if (!rooms.canJoinRoom(invitation.roomId)) status = "unavailable";
    else {
        const auto result = rooms.createInvitation(invitation.fromSession, invitation.roomId, now);
        if (const auto* issued = std::get_if<RoomRegistry::CreatedInvitation>(&result)) {
            deliveries.append({sessionId, event("contact.accepted", {{"invitationId", id},
                {"roomId", invitation.roomId}, {"meshId", invitation.meshId},
                {"token", issued->token}})});
            status = "accepted";
        } else {
            status = "unavailable";
        }
    }
    deliveries += finish(invitation, status);
    return deliveries;
}

QVector<Delivery> PresenceRegistry::maintainInvitations(const RoomRegistry& rooms, qint64 now) {
    QVector<Delivery> deliveries;
    for (auto it = knocks_.begin(); it != knocks_.end();) {
        QString reason;
        if (now >= it->expiresAt) reason = "expired";
        else if (!mutual(it->fromSession, it->toSession) || profiles_.value(it->toSession).meshId != it->meshId) reason = "cancelled";
        else if (profiles_.value(it->fromSession).busy || !rooms.roomForSession(it->fromSession).isEmpty()) reason = "busy";
        if (reason.isEmpty()) { ++it; continue; }
        deliveries += finishKnock(it.value(), reason);
        it = knocks_.erase(it);
    }
    for (auto it = invitations_.begin(); it != invitations_.end();) {
        QString reason;
        if (now >= it->expiresAt) {
            reason = "expired";
        } else if (!mutual(it->fromSession, it->toSession) ||
                   rooms.roomForSession(it->fromSession) != it->roomId) {
            reason = "cancelled";
        } else if (profiles_.value(it->toSession).busy ||
                   !rooms.roomForSession(it->toSession).isEmpty()) {
            reason = "busy";
        }
        if (reason.isEmpty()) {
            ++it;
            continue;
        }

        deliveries += finish(it.value(), reason);
        it = invitations_.erase(it);
    }
    for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
        for (auto invite = it->lastInvite.begin(); invite != it->lastInvite.end();) {
            if (now - invite.value() >= 10000) {
                invite = it->lastInvite.erase(invite);
            } else {
                ++invite;
            }
        }
    }
    return deliveries;
}

QVector<Delivery> PresenceRegistry::updateSnapshots() {
    QVector<Delivery> deliveries;
    for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
        auto ids = it->known.values();
        std::sort(ids.begin(), ids.end());
        QJsonArray snapshot;
        for (const auto& id : ids) {
            snapshot.append(QJsonObject{{"identityId", id},
                {"online", mutual(it.key(), sessionsByIdentity_.value(id))},
                {"inConversation", mutual(it.key(), sessionsByIdentity_.value(id)) &&
                    !profiles_.value(sessionsByIdentity_.value(id)).meshId.isEmpty()}});
        }

        if (!it->published || snapshot != it->snapshot) {
            it->published = true;
            it->snapshot = snapshot;
            deliveries.append({it.key(), event("presence.snapshot", {{"peers", snapshot}})});
        }
    }
    return deliveries;
}

QVector<Delivery> PresenceRegistry::disconnect(const QString& sessionId, const RoomRegistry& rooms, qint64 now) {
    const auto found = profiles_.find(sessionId);
    const bool hadProfile = found != profiles_.end();
    if (hadProfile) {
        sessionsByIdentity_.remove(found->identityId);
        profiles_.erase(found);
    }
    auto deliveries = maintainInvitations(rooms, now);
    if (hadProfile) {
        deliveries += updateSnapshots();
    }
    return deliveries;
}

void PresenceRegistry::clear() {
    profiles_.clear();
    sessionsByIdentity_.clear();
    invitations_.clear();
    knocks_.clear();
}

} // namespace tmc::server
