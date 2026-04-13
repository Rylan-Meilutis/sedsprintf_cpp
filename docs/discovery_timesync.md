# Discovery And Time Sync

## Discovery

Discovery packets advertise:
- reachable endpoints
- reachable time sync source identifiers

Discovery is used to:
- avoid blind flooding when a route is known
- export topology snapshots
- constrain link-local traffic to link-local-enabled sides

Relevant files:
- `src/discovery.hpp`
- `src/discovery.cpp`
- `src/discovery_helpers.hpp`

## Time Sync

Time sync support includes:
- announce packets
- request / response exchange
- discovered source routing
- local network-time setters and clearing APIs

The router can:
- advertise itself as a time source
- follow discovered remote sources
- maintain a current network-time anchor

Relevant files:
- `src/timesync.hpp`
- `src/timesync.cpp`

The C++ port mirrors the Rust built-in internal endpoint/type behavior for:
- `SEDS_EP_TIME_SYNC`
- `SEDS_DT_TIME_SYNC_ANNOUNCE`
- `SEDS_DT_TIME_SYNC_REQUEST`
- `SEDS_DT_TIME_SYNC_RESPONSE`
