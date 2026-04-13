#include "packet.hpp"

#include "internal.hpp"

#include <cstring>
#include <stdexcept>

namespace seds {

Packet::Packet(uint32_t ty, std::vector<uint32_t> endpoints, std::string sender, uint64_t timestamp,
               std::span<const uint8_t> payload)
    : ty_(ty), endpoints_(std::move(endpoints)), sender_(std::move(sender)), timestamp_(timestamp), payload_(payload) {
    if (!valid_type(ty_) || endpoints_.empty() || !validate_payload(ty_, payload_.size())) {
        throw std::invalid_argument("invalid packet");
    }
}

Packet Packet::from_view(const SedsPacketView& view) {
    PacketData pkt;
    if (!packet_from_view(&view, pkt)) {
        throw std::invalid_argument("invalid packet view");
    }
    return {pkt.ty, pkt.endpoints, pkt.sender, pkt.timestamp, pkt.payload};
}

std::optional<Packet> Packet::deserialize(std::span<const uint8_t> bytes) {
    auto pkt = deserialize_packet(bytes.data(), bytes.size());
    if (!pkt.has_value()) {
        return std::nullopt;
    }
    return Packet(pkt->ty, pkt->endpoints, pkt->sender, pkt->timestamp, pkt->payload);
}

Packet Packet::from_f32_slice(uint32_t ty, std::span<const float> values, std::vector<uint32_t> endpoints,
                              uint64_t timestamp, std::string sender) {
    std::vector<uint8_t> payload(values.size() * sizeof(float));
    std::memcpy(payload.data(), values.data(), payload.size());
    return {ty, std::move(endpoints), std::move(sender), timestamp, payload};
}

SedsPacketView Packet::view() const {
    return SedsPacketView{
        .ty = ty_,
        .data_size = payload_.size(),
        .sender = sender_.c_str(),
        .sender_len = sender_.size(),
        .endpoints = endpoints_.data(),
        .num_endpoints = endpoints_.size(),
        .timestamp = timestamp_,
        .payload = payload_.bytes().data(),
        .payload_len = payload_.size(),
    };
}

std::vector<uint8_t> Packet::serialize() const {
    const PacketData pkt{ty_, sender_, endpoints_, timestamp_,
                         std::vector<uint8_t>(payload_.bytes().begin(), payload_.bytes().end())};
    return serialize_packet(pkt);
}

std::string Packet::header_string() const {
    const auto pkt_view = view();
    return packet_header_string(pkt_view);
}

std::string Packet::to_string() const {
    const auto pkt_view = view();
    return packet_to_string(pkt_view);
}

}  // namespace seds
