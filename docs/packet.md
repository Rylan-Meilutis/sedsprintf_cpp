# Packet Module

The packet layer is the core payload abstraction.

It is responsible for:
- holding sender, endpoint list, timestamp, and payload bytes
- validating payload width/count against schema metadata
- formatting packets and headers for logs/debugging
- converting between owned C++ packet objects and C ABI packet views

Relevant files:
- `src/packet.hpp`
- `src/packet.cpp`
- `src/packet_api.cpp`

Related helpers:
- `src/small_payload.hpp`
- `src/serialize.hpp`

Important behavior:
- empty endpoint lists are rejected for normal packets
- string payload formatting trims trailing NULs where appropriate
- packet string output is kept stable for tests and tooling
