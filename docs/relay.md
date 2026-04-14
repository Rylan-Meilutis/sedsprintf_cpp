# Relay Module

The relay forwards packets between sides without local endpoint ownership.

Responsibilities:
- receive packets from a source side and forward to selected destination sides
- learn and advertise discovery topology
- apply side-level ingress/egress controls
- maintain reliable forwarding state for reliable message types

Built-in control traffic:
- discovery advertisements flow on the reserved `DISCOVERY` endpoint
- relay forwarding keeps those announcements selective rather than flooding every side

Reliable forwarding:
- relay egress reliability is also schema-driven
- ordered reliable traffic preserves sequence across relay hops
- ACK-driven advancement and retransmit behavior match the Rust transport semantics more closely

Route selection supports:
- fanout
- weighted distribution
- failover

Discovery behavior:
- learned routes are used for selective forwarding
- announce traffic exports full discovered topology graphs
- expired routes are pruned and route mode falls back to remaining valid paths
- exported snapshots include top-level router graph data plus per-side announcer detail

Relevant files:
- `src/relay.hpp`
- `src/relay.cpp`
- `src/relay_api.cpp`
- `src/router_core.cpp`
