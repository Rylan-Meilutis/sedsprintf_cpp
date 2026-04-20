#pragma once

#include "internal.hpp"

#include <optional>
#include <span>
#include <vector>

namespace seds {

inline std::vector<uint8_t> serialize(const Packet& packet) {
    return packet.serialize();
}

inline std::optional<Packet> deserialize(std::span<const uint8_t> bytes) {
    return Packet::deserialize(bytes);
}

inline std::optional<FrameInfoLite> peek_envelope(std::span<const uint8_t> bytes, bool verify_crc = true) {
    return peek_frame_info(bytes.data(), bytes.size(), verify_crc);
}

}  // namespace seds
