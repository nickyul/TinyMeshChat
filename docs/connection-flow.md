# Connection flow

Offers create a reliable ordered DataChannel, begin ICE gathering, and are exported only after `GatheringState::Complete`; the final `localDescription()` therefore contains gathered candidates (non-trickle ICE). The receiver sets the remote offer, gathers an answer, and exports it once. The offerer imports the matching room/connection answer.

Any member may create the manual invitation. Once its DataChannel opens, both sides exchange the bounded room roster. Every peer compares IDs and the lexicographically smaller ID creates an offer for each missing pair. Service offer/answer packets are flooded through the connected graph using route IDs and hop limits. Ordinary `chat.message` packets are never relayed.

After a restart or total channel failure, any reachable member generates a fresh invitation. Stale closed transport state is discarded while the room, peer records, local history, and pending delivery rows remain intact. Once one new channel opens, the roster, remaining mesh, recent history, and pending messages recover automatically.
