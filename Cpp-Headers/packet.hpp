#pragma once

#include "small_payload.hpp"
#include "sedsprintf.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace seds {

class Packet {
public:
    Packet() = default;
    Packet(uint32_t ty, std::vector<uint32_t> endpoints, std::string sender, uint64_t timestamp,
           std::span<const uint8_t> payload);

    static Packet from_view(const SedsPacketView& view);
    static std::optional<Packet> deserialize(std::span<const uint8_t> bytes);
    static Packet from_f32_slice(uint32_t ty, std::span<const float> values, std::vector<uint32_t> endpoints,
                                 uint64_t timestamp, std::string sender = "CPP");

    [[nodiscard]] uint32_t type() const { return ty_; }
    [[nodiscard]] const std::vector<uint32_t>& endpoints() const { return endpoints_; }
    [[nodiscard]] const std::string& sender() const { return sender_; }
    [[nodiscard]] uint64_t timestamp() const { return timestamp_; }
    [[nodiscard]] std::span<const uint8_t> payload() const { return payload_.bytes(); }

    [[nodiscard]] SedsPacketView view() const;
    [[nodiscard]] std::vector<uint8_t> serialize() const;
    [[nodiscard]] std::string header_string() const;
    [[nodiscard]] std::string to_string() const;

private:
    uint32_t ty_{0};
    std::vector<uint32_t> endpoints_;
    std::string sender_;
    uint64_t timestamp_{0};
    SmallPayload payload_;
};

}  // namespace seds
