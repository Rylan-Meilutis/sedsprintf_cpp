# Router Module

The router owns local endpoint handlers and side links.

Responsibilities:
- deliver packets to matching local handlers
- transmit to sides according to route policy
- learn discovery routes from incoming discovery traffic
- emit discovery and time sync control traffic when enabled
- manage reliable delivery state for side links

Built-in internal traffic:
- discovery packets use the reserved `DISCOVERY` endpoint
- time sync packets use the reserved `TIME_SYNC` endpoint
- those reserved local endpoints are not user-registerable through the C ABI

Reliable transport:
- reliability is schema-driven per type rather than a flat boolean
- `Ordered` frames enforce in-order delivery with retransmit/ACK
- `Unordered` frames still ACK and retransmit, but do not gate delivery on sequence order

Routing modes:
- `Fanout`
- `Weighted`
- `Failover`

Route controls exist at two levels:
- base per-source route overrides
- typed per-source/per-type route overrides

Current parity points mirrored from Rust:
- reserved internal endpoints (`DISCOVERY`, `TIME_SYNC`) cannot be user-registered
- side removal updates discovery state and routing behavior
- typed routes still respect base-route disables
- discovered routes participate in weighted/failover selection
- `DISCOVERY_TOPOLOGY` propagates transitive board graphs
- exported topology includes top-level `routers` plus per-side `announcers`

Relevant files:
- `src/router.hpp`
- `src/router.cpp`
- `src/router_api.cpp`
- `src/router_core.cpp`
