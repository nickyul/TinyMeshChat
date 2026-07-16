# Connection flow

Offers create a reliable ordered DataChannel, begin ICE gathering, and are exported only after `GatheringState::Complete`; the final `localDescription()` therefore contains gathered candidates (non-trickle ICE). The receiver sets the remote offer, gathers an answer, and exports it once. The offerer imports the matching room/connection answer.

Any member may create the manual invitation. Once its DataChannel opens, both sides exchange the bounded room roster. Every peer compares IDs and the lexicographically smaller ID creates an offer for each missing pair. Service offer/answer packets are flooded through the connected graph using route IDs and hop limits. Ordinary `chat.message` packets are never relayed.

After a connection failure, a member whose process still has the room in memory can generate a fresh invitation; the roster then rebuilds missing direct links. After an application restart all room state is gone, so the user creates a new room or imports an invitation from a still-running member. Earlier messages and pending deliveries are never recovered or replayed.
